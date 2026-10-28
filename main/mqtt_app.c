/* MQTT over TLS 客户端实现（HiveMQ Cloud）。 */

#include "mqtt_app.h"
#include "cmd.h"
#include "net_config.h"
#include "wifi_mgr.h"

#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "mqtt_client.h"

#define TAG "mqtt"

static esp_mqtt_client_handle_t s_client;
static volatile bool s_connected;
static bool s_ever_connected;
static char s_client_id[40];

/* 收到控制指令后置位：主循环看到就立刻发一条遥测，
 * 免得 APP 要等 30 秒的下一个上报周期才能看到控制结果。 */
static volatile bool s_publish_requested;

/* 指令回执缓冲区：静态而非栈上，因为 rule:list 的输出可达数百字节。
 * 只在 MQTT 事件回调（同一个任务）里使用，无竞争。 */
static char s_ack[512];

/* ---------------- 入站指令重组 ---------------- */

/* 入站指令缓冲上限。
 *
 * 取值依据：cmd.c 的规则分支用 `char arg[192]` 接收 "rule:" 之后的原文，
 * 即它最多接受 5 + 191 = 196 字节的完整指令（超出则显式报 "command too long"）。
 * 取 256 可保证「凡是 cmd.c 会接受、或会显式拒绝的指令，都能原样送达本层」，
 * 不会被本层提前破坏。 */
#define MQTT_CMD_MAX 256

typedef enum {
    MQTT_RX_NEED_MORE = 0,  /* 分片未收齐，继续等 */
    MQTT_RX_COMPLETE,       /* 已收齐：buf 中是完整指令（NUL 结尾） */
    MQTT_RX_REJECTED,       /* 超长或分片不一致：已丢弃，**不可执行** */
} mqtt_rx_result_t;

typedef struct {
    char buf[MQTT_CMD_MAX];
    int  len;
    bool rejected;
    bool reject_reported;   /* 本条消息的拒绝回执是否已发（避免逐分片刷屏） */
} mqtt_rx_t;

static mqtt_rx_t s_rx;      /* 只在 MQTT 事件回调（单一任务）里使用，无竞争 */

/* 喂入一个 MQTT_EVENT_DATA 分片，返回是否已收齐。
 *
 * 为什么必须重组：esp-mqtt 在**载荷超过接收缓冲**时，会把一条 publish 拆成
 * 多个 MQTT_EVENT_DATA 事件，每片带 current_data_offset / total_data_len
 * （见 managed_components/espressif__mqtt/mqtt_client.c 的 deliver_publish：
 * total_data_len 恒被设置，未分片时 offset=0）。
 *
 * 为什么必须"显式拒绝"而不是"截断后执行"：
 *   cmd.c 内部已有 "command too long" 守卫（规则参数 192 字节上限），
 *   但那个守卫**保护不了"到达它之前就已经被截短"的输入**——本层若按固定长度
 *   硬截，被截短的指令就"变短了"，内层守卫不再触发，残缺内容被当作完整指令执行。
 *   例如 `hold=600` 的尾部被截掉会解析成 hold=0（立即动作），规则含义彻底改变。
 *   所以本层要么**完整送达**，要么**明确拒绝**，绝不静默截断。
 *
 * 这条是本项目"绝不静默截断"原则在 MQTT 侧的落地（BLE 侧见 ble_svc 的分片
 * notify，cmd.c 侧见 rule 参数的长度守卫）。 */
static mqtt_rx_result_t mqtt_rx_feed(mqtt_rx_t *rx, int offset, int total,
                                     const char *data, int len)
{
    /* offset==0 = 新消息的第一片：重置状态。
     * esp-mqtt 每条 publish 都从 0 开始，因此这是可靠的"新消息"信号。 */
    if (offset == 0) {
        rx->len = 0;
        rx->rejected = false;
        rx->reject_reported = false;
    }
    if (rx->rejected) return MQTT_RX_REJECTED;

    /* 超长（或空载荷）：直接拒绝，一个字节都不执行 */
    if (total <= 0 || total >= MQTT_CMD_MAX) {
        rx->rejected = true;
        return MQTT_RX_REJECTED;
    }

    /* 分片必须按序、不重叠、不越界；否则状态不可信，宁可丢弃也不拼出错误指令 */
    if (offset != rx->len || len < 0 || rx->len + len > total) {
        rx->rejected = true;
        return MQTT_RX_REJECTED;
    }

    /* data 在 data_len==0 时可为 NULL（见 deliver_publish），故加长度判断 */
    if (len > 0) memcpy(rx->buf + rx->len, data, len);
    rx->len += len;

    if (rx->len < total) return MQTT_RX_NEED_MORE;

    rx->buf[rx->len] = '\0';   /* total < MQTT_CMD_MAX，故一定有位置放 NUL */
    return MQTT_RX_COMPLETE;
}

/* MQTT CONNACK 拒绝码 -> 可读原因。凭据/授权问题会走这里，
 * 与传输层错误（TLS/网络）是完全不同的排查方向。 */
static const char *conn_refused_str(int code)
{
    switch (code) {
    case 0: return "accepted";
    case 1: return "unacceptable protocol version";
    case 2: return "identifier rejected (client_id not allowed)";
    case 3: return "server unavailable";
    case 4: return "BAD USERNAME OR PASSWORD";
    case 5: return "NOT AUTHORIZED (check Access Management topic permissions)";
    default: return "unknown return code";
    }
}

/* 把 esp-mqtt 的错误句柄翻译成可操作的结论 */
static void report_error(esp_mqtt_event_handle_t e)
{
    esp_mqtt_error_codes_t *ec = e->error_handle;
    if (ec == NULL) {
        ESP_LOGE(TAG, "mqtt error event without detail");
        return;
    }

    switch (ec->error_type) {
    case MQTT_ERROR_TYPE_TCP_TRANSPORT:
        /* 传输层：可能是 TLS 握手/证书问题，也可能是网络不可达 */
        ESP_LOGE(TAG, "transport error: tls_esp_err=%s tls_stack_err=0x%x cert_verify_flags=0x%x sock_errno=%d",
                 esp_err_to_name(ec->esp_tls_last_esp_err), ec->esp_tls_stack_err,
                 ec->esp_tls_cert_verify_flags, ec->esp_transport_sock_errno);
        if (ec->esp_tls_cert_verify_flags != 0xff && ec->esp_tls_cert_verify_flags != 0) {
            ESP_LOGE(TAG, "  => SERVER CERTIFICATE VERIFICATION FAILED");
            if (!wifi_mgr_time_synced()) {
                ESP_LOGE(TAG, "  => system clock NOT synced. TLS validates cert validity dates;");
                ESP_LOGE(TAG, "     with clock at 1970 the cert looks 'not yet valid'. Fix SNTP first.");
            } else {
                ESP_LOGE(TAG, "  => CA bundle may not cover this broker's CA, or broker hostname mismatch");
            }
        } else if (ec->esp_tls_stack_err != 0) {
            ESP_LOGE(TAG, "  => TLS handshake failed (see tls_stack_err above)");
        } else if (ec->esp_transport_sock_errno != 0) {
            ESP_LOGE(TAG, "  => socket/network level failure (DNS, unreachable, timeout)");
        }
        break;

    case MQTT_ERROR_TYPE_CONNECTION_REFUSED:
        ESP_LOGE(TAG, "broker REFUSED connection: return code=%d (%s)",
                 ec->connect_return_code, conn_refused_str(ec->connect_return_code));
        if (ec->connect_return_code == 4) {
            ESP_LOGE(TAG, "  => check SECRET_MQTT_USERNAME / SECRET_MQTT_PASSWORD in secrets.h");
        } else if (ec->connect_return_code == 5) {
            ESP_LOGE(TAG, "  => credentials OK but not permitted; check topic permissions");
        }
        break;

    case MQTT_ERROR_TYPE_SUBSCRIBE_FAILED:
        ESP_LOGE(TAG, "subscribe failed");
        break;

    default:
        ESP_LOGE(TAG, "mqtt error type=%d", (int)ec->error_type);
        break;
    }
}

static void mqtt_event_handler(void *args, esp_event_base_t base, int32_t id, void *data)
{
    (void)args; (void)base;
    esp_mqtt_event_handle_t e = (esp_mqtt_event_handle_t)data;

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        ESP_LOGI(TAG, "CONNECTED to broker as \"%s\"", s_client_id);
        /* 连上后主动发一次在线状态：这样即使 broker 不支持 retained，
         * APP 订阅后也能很快拿到设备状态（不依赖 retained 的设计）。
         * retain 同样设为 0，理由见上面遗嘱处。 */
        esp_mqtt_client_publish(s_client, SMARTFARM_TOPIC_STATUS,
                                "{\"online\":true}", 0, 1, 0);
        /* 订阅指令主题：APP 远程控制走这里 */
        esp_mqtt_client_subscribe(s_client, SMARTFARM_TOPIC_CMD, 1);
        s_ever_connected = true;
        break;

    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "disconnected from broker (auto-reconnect in progress)");
        break;

    case MQTT_EVENT_ERROR:
        report_error(e);
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "subscribed to %s", SMARTFARM_TOPIC_CMD);
        break;

    case MQTT_EVENT_DATA: {
        /* 收到指令。data 不一定以 '\0' 结尾，故按长度比对。
         *
         * ⚠️ 不能只看 e->data_len：esp-mqtt 在载荷超过接收缓冲时会**分片**，
         *    一条 publish 会产生多个 MQTT_EVENT_DATA。旧实现把每片当独立指令执行，
         *    并把载荷硬截到 127 字节——两者都会让残缺内容被当成完整指令执行。
         *    现统一交给 mqtt_rx_feed 重组，收齐后才执行（理由见该函数注释）。 */
        char topic[96] = { 0 };
        int tlen = e->topic_len < (int)sizeof(topic) - 1 ? e->topic_len : (int)sizeof(topic) - 1;
        memcpy(topic, e->topic, tlen);

        if (strcmp(topic, SMARTFARM_TOPIC_CMD) != 0) {
            /* 本设备只订阅 cmd 主题，收到别的主题说明配置与预期不符 */
            ESP_LOGW(TAG, "RX on unexpected topic \"%s\" (%d bytes), ignored", topic, e->data_len);
            break;
        }

        mqtt_rx_result_t rr = mqtt_rx_feed(&s_rx, e->current_data_offset,
                                           e->total_data_len, e->data, e->data_len);
        if (rr == MQTT_RX_NEED_MORE) break;     /* 还有后续分片，等收齐 */

        if (rr == MQTT_RX_REJECTED) {
            /* 只回执一次：本条消息后续分片会继续返回 REJECTED，别逐片刷屏。
             * 关键点——**明确告诉 APP"没执行"**，而不是让它猜。
             * 绝不执行被截断或分片不完整的内容。 */
            if (!s_rx.reject_reported) {
                s_rx.reject_reported = true;
                ESP_LOGE(TAG, "cmd payload REJECTED: %d bytes (limit %d) or inconsistent fragments; "
                              "NOT executed", e->total_data_len, MQTT_CMD_MAX - 1);
                char rej[128];
                snprintf(rej, sizeof(rej),
                         "{\"ack\":\"mqtt\",\"ok\":0,\"err\":\"command too long or malformed\","
                         "\"len\":%d,\"max\":%d}", e->total_data_len, MQTT_CMD_MAX - 1);
                esp_mqtt_client_publish(s_client, SMARTFARM_TOPIC_ACK, rej, 0, 1, 0);
            }
            break;
        }

        /* 到这里 s_rx.buf 一定是**完整**指令。
         * 与 BLE 走**同一个解析器**：两条通道的控制语义完全一致。
         * allow_provisioning=false：配网只允许本地（BLE）做，理由见 cmd.h。
         *
         * ⚠️ 日志必须走 cmd_mask_for_log：远程通道虽然**拒绝**配网指令，
         * 但拒绝发生在 cmd_execute 内部，而日志在它**之前**就已打印——
         * 原文打印会把被拒绝的 wifi:pass:<密码> 照样写进串口日志。 */
        char shown[96];
        cmd_mask_for_log(s_rx.buf, shown, sizeof(shown));
        ESP_LOGI(TAG, "RX cmd payload=%s", shown);
        cmd_status_t st = cmd_execute(s_rx.buf, s_ack, sizeof(s_ack), false);
        ESP_LOGI(TAG, "cmd \"%s\" -> %s", shown, s_ack);

        esp_mqtt_client_publish(s_client, SMARTFARM_TOPIC_ACK, s_ack, 0, 1, 0);

        /* 控制类指令立刻回一条最新遥测，别让 APP 等 30 秒的下一个周期。
         * 这里只置位请求，实际组包由主循环做（它持有传感器数据）。 */
        if (st == CMD_OK_PUSH_TELEM || st == CMD_OK) {
            s_publish_requested = true;
        }
        break;
    }

    default:
        break;
    }
}

esp_err_t mqtt_app_start(void)
{
    if (s_client) return ESP_OK;

    if (SMARTFARM_SECRETS_PRESENT == 0 || SECRET_MQTT_URI[0] == '\0') {
        ESP_LOGE(TAG, "no MQTT config: create main/secrets.h from secrets.h.example");
        return ESP_ERR_INVALID_STATE;
    }
    if (!wifi_mgr_is_connected()) {
        ESP_LOGW(TAG, "WiFi not connected yet");
        return ESP_ERR_INVALID_STATE;
    }
    if (!wifi_mgr_time_synced()) {
        ESP_LOGW(TAG, "time not synced yet - TLS validates cert dates, waiting is required");
        return ESP_ERR_INVALID_STATE;
    }

    /* client_id 必须唯一，否则同名连接会互相踢掉。用 STA MAC 后缀保证唯一。 */
    uint8_t mac[6] = { 0 };
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(s_client_id, sizeof(s_client_id), "smartfarm-%s-%02x%02x%02x",
             SMARTFARM_NODE_ID, mac[3], mac[4], mac[5]);

    const esp_mqtt_client_config_t cfg = {
        .broker = {
            .address.uri = SECRET_MQTT_URI,
            /* 用内置 CA 证书包做服务器验证：不硬编码证书，CA 轮换无需重烧设备 */
            .verification.crt_bundle_attach = esp_crt_bundle_attach,
        },
        .credentials = {
            .username  = SECRET_MQTT_USERNAME,
            .client_id = s_client_id,
            .authentication.password = SECRET_MQTT_PASSWORD,
        },
        .session = {
            .keepalive = 60,
            .protocol_ver = MQTT_PROTOCOL_V_3_1_1,
            /* 遗嘱：设备掉线时 broker 代发在线状态。
             *
             * ⚠️ retain 故意设为 0：broker 在**连接时**就要授权遗嘱主题，
             * 若凭据不允许 retained 消息，CONNECT 会被直接拒绝（返回码 5 未授权）。
             * 本项目设计本来就不依赖 retained（设备连上后会主动发一次状态），
             * 因此去掉它可少一个授权依赖。 */
            .last_will = {
                .topic  = SMARTFARM_TOPIC_STATUS,
                .msg    = "{\"online\":false}",
                .qos    = 1,
                .retain = 0,
            },
        },
        .network = {
            .reconnect_timeout_ms = 5000,   /* 断线 5 秒后重连 */
            .timeout_ms           = 10000,
        },
    };

    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        return ESP_FAIL;
    }
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                                   mqtt_event_handler, NULL));
    esp_err_t err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_mqtt_client_start failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "MQTT client started (non-blocking) -> %s", SECRET_MQTT_URI);
    ESP_LOGI(TAG, "  client_id=%s  status=%s  cmd=%s", s_client_id,
             SMARTFARM_TOPIC_STATUS, SMARTFARM_TOPIC_CMD);
    return ESP_OK;
}

bool mqtt_app_is_connected(void)
{
    return s_connected;
}

esp_err_t mqtt_app_publish(const char *topic, const char *payload, int qos, int retain)
{
    if (!s_client || !s_connected) return ESP_ERR_INVALID_STATE;
    int msg_id = esp_mqtt_client_publish(s_client, topic, payload, 0, qos, retain);
    return (msg_id < 0) ? ESP_FAIL : ESP_OK;
}

esp_err_t mqtt_app_publish_telemetry(const char *json)
{
    return mqtt_app_publish(SMARTFARM_TOPIC_TELEMETRY, json, 1, 0);
}

bool mqtt_app_take_publish_request(void)
{
    if (!s_publish_requested) return false;
    s_publish_requested = false;
    return true;
}

/* ---------------- 自检 ---------------- */

/* 目标板上自检**入站指令重组**。
 *
 * ⚠️ 纯逻辑：**不联网、不订阅、不发布任何消息**，也不碰 s_client。
 *
 * 盯住的是一条安全不变量：**超长或分片不一致的远端载荷绝不产生可执行指令**。
 * 这是真实攻击面——cmd 主题对任何持有 MQTT 凭据的客户端开放；
 * 若超长载荷被截断后执行，可能生成与发送方意图**完全不同**的规则
 * （典型：`hold=600` 尾部被截掉 → `hold=0` → 条件一满足就立即动作）。 */
void mqtt_app_selftest(void)
{
    int fail = 0;

    /* 一条真实长度的规则指令，用于验证逐字节还原 */
    static const char MSG[] =
        "rule:add mode=condition relay=2 src=water_t op=< val=5.0 hold=600 enabled=1";
    const int N = (int)(sizeof(MSG) - 1);

    /* 1) 单片完整消息（esp-mqtt 未分片时的形态：offset=0、total=全长） */
    {
        mqtt_rx_t rx = { 0 };
        mqtt_rx_result_t r = mqtt_rx_feed(&rx, 0, N, MSG, N);
        if (r != MQTT_RX_COMPLETE || rx.len != N || strcmp(rx.buf, MSG) != 0) {
            ESP_LOGE(TAG, "mqtt selftest FAIL: single-chunk message not reassembled intact");
            fail++;
        }
    }

    /* 2) 切成 3 片（模拟载荷超过接收缓冲）：必须**逐字节**还原 */
    {
        mqtt_rx_t rx = { 0 };
        const int C1 = 17, C2 = 53;
        mqtt_rx_result_t r1 = mqtt_rx_feed(&rx, 0,  N, MSG,      C1);
        mqtt_rx_result_t r2 = mqtt_rx_feed(&rx, C1, N, MSG + C1, C2 - C1);
        mqtt_rx_result_t r3 = mqtt_rx_feed(&rx, C2, N, MSG + C2, N - C2);
        if (r1 != MQTT_RX_NEED_MORE || r2 != MQTT_RX_NEED_MORE ||
            r3 != MQTT_RX_COMPLETE  || rx.len != N || strcmp(rx.buf, MSG) != 0) {
            ESP_LOGE(TAG, "mqtt selftest FAIL: fragmented message not reassembled exactly");
            fail++;
        }
    }

    /* 3) 超长载荷：必须拒绝，且**后续分片保持拒绝**（不能拼出一条残缺指令） */
    {
        static char big[MQTT_CMD_MAX + 40];
        mqtt_rx_t rx = { 0 };
        memset(big, 'A', sizeof(big));
        big[sizeof(big) - 1] = '\0';
        const int B = (int)(sizeof(big) - 1);

        mqtt_rx_result_t r1 = mqtt_rx_feed(&rx, 0, B, big, B);
        mqtt_rx_result_t r2 = mqtt_rx_feed(&rx, 8, B, big, 8);
        if (r1 != MQTT_RX_REJECTED || r2 != MQTT_RX_REJECTED) {
            ESP_LOGE(TAG, "mqtt selftest FAIL: oversized payload was NOT rejected");
            fail++;
        }
    }

    /* 4) 空载荷：同样拒绝，不应走到 cmd_execute("") */
    {
        mqtt_rx_t rx = { 0 };
        if (mqtt_rx_feed(&rx, 0, 0, NULL, 0) != MQTT_RX_REJECTED) {
            ESP_LOGE(TAG, "mqtt selftest FAIL: empty payload was NOT rejected");
            fail++;
        }
    }

    /* 5) 分片乱序/重叠：状态不可信 -> 拒绝，而不是拼出一条错误的指令 */
    {
        mqtt_rx_t rx = { 0 };
        (void)mqtt_rx_feed(&rx, 0, N, MSG, 10);
        if (mqtt_rx_feed(&rx, 99, N, MSG + 10, 10) != MQTT_RX_REJECTED) {
            ESP_LOGE(TAG, "mqtt selftest FAIL: out-of-order fragment was NOT rejected");
            fail++;
        }
    }

    /* 6) 被拒绝后收到新消息（offset 回到 0）：必须能复位并正常收齐 */
    {
        mqtt_rx_t rx = { 0 };
        (void)mqtt_rx_feed(&rx, 0, 0, NULL, 0);          /* 先制造一次拒绝 */
        mqtt_rx_result_t r = mqtt_rx_feed(&rx, 0, N, MSG, N);
        if (r != MQTT_RX_COMPLETE || strcmp(rx.buf, MSG) != 0) {
            ESP_LOGE(TAG, "mqtt selftest FAIL: state did not recover after a rejected message");
            fail++;
        }
    }

    /* 7) 边界：正好 MQTT_CMD_MAX-1 字节必须**被接受**（别把合法上限也拒掉） */
    {
        static char edge[MQTT_CMD_MAX];
        mqtt_rx_t rx = { 0 };
        memset(edge, 'B', sizeof(edge));
        edge[sizeof(edge) - 1] = '\0';
        const int E = MQTT_CMD_MAX - 1;
        mqtt_rx_result_t r = mqtt_rx_feed(&rx, 0, E, edge, E);
        if (r != MQTT_RX_COMPLETE || rx.len != E || strcmp(rx.buf, edge) != 0) {
            ESP_LOGE(TAG, "mqtt selftest FAIL: payload at the max-1 boundary was rejected");
            fail++;
        }
    }

    if (fail == 0) {
        ESP_LOGI(TAG, "mqtt selftest OK (reassembly: single/3-fragment exact; "
                      "oversized/empty/out-of-order rejected; recovers; boundary accepted)");
    } else {
        ESP_LOGE(TAG, "mqtt selftest FAILED (%d case(s))", fail);
    }
}
