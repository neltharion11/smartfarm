/* BLE GATT 外设实现（NimBLE）。 */

#include "ble_svc.h"
#include "cmd.h"
#include "net_config.h"
#include "wifi_mgr.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#define TAG "ble"

/* 自定义 128 位 UUID 的统一构造。
 *
 * BLE_UUID128_INIT 按**小端**取字节，即与人可读字符串相反：
 *   人可读  f0a00001-8b3c-4d5e-9f10-2a3b4c5d6e7f
 *   字节序  7f 6e 5d 4c 3b 2a | 10 9f | 5e 4d | 3c 8b | 01 00 a0 f0
 * 末尾 4 字节承载特征编号（01..05），故用宏参数化以免手写 16 个字节出错。 */
#define SMARTFARM_UUID128(n) BLE_UUID128_INIT(                     \
        0x7f, 0x6e, 0x5d, 0x4c, 0x3b, 0x2a, 0x10, 0x9f,            \
        0x5e, 0x4d, 0x3c, 0x8b, (n), 0x00, 0xa0, 0xf0)

static const ble_uuid128_t uuid_svc       = SMARTFARM_UUID128(0x01);
static const ble_uuid128_t uuid_telemetry = SMARTFARM_UUID128(0x02);
static const ble_uuid128_t uuid_command   = SMARTFARM_UUID128(0x03);
static const ble_uuid128_t uuid_status    = SMARTFARM_UUID128(0x04);
static const ble_uuid128_t uuid_wifi      = SMARTFARM_UUID128(0x05);

static uint8_t  s_own_addr_type;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

/* ⚠️ 初始值必须是 BLE 规范的默认 ATT MTU(23)，而不是 0。
 * 踩过的坑：中心设备（蓝牙助手）不发起 MTU 交换时 BLE_GAP_EVENT_MTU 根本不会触发，
 * 若此处为 0，所有基于 MTU 的长度判断都会被跳过，超长数据会被 ATT 层**静默截断**
 * 成 20 字节 —— 表现为"连上了但 JSON 只到一半"。按规范默认值起步即可避免。 */
static uint16_t s_mtu = 23;
static uint16_t s_telemetry_val_handle;
static uint16_t s_cmd_val_handle;
static uint16_t s_wifi_val_handle;
static bool     s_notify_enabled;       /* 遥测特征的订阅状态 */
static bool     s_cmd_notify_enabled;   /* 指令特征的订阅状态 */
static bool     s_wifi_notify_enabled;  /* 配网特征的订阅状态 */

static char s_dev_name[24];
static char s_telemetry[224] = "{\"state\":\"no data yet\"}";
static char s_status[96]     = "{\"fw\":\"" SMARTFARM_FW_VERSION "\",\"state\":\"boot\"}";

/* MTU 不足时只告警一次，避免每秒刷屏 */
static bool s_mtu_warned;

static int  gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                        struct ble_gatt_access_ctxt *ctxt, void *arg);
static int  gap_event_cb(struct ble_gap_event *event, void *arg);
static void advertise(void);
static void on_wifi_state(const char *json);

/* 指令回执缓冲区。取静态而非栈上：rule:list 的输出可达数百字节，
 * 放 BLE 主机任务的栈上会平白吃掉栈空间。
 * gatt_access 只在 NimBLE 主机任务里跑，单线程访问，无竞争。 */
static char s_ack[512];

/* ---------------- GATT 服务表 ---------------- */

static const struct ble_gatt_svc_def gatt_svcs[] = {
    /* 标准设备信息服务：让蓝牙助手一连接就能读到有意义的内容，
     * 便于在写任何业务逻辑前先验证 GATT 通路。 */
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(0x180A),
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = BLE_UUID16_DECLARE(0x2A29), .access_cb = gatt_access,
              .flags = BLE_GATT_CHR_F_READ },   /* Manufacturer Name */
            { .uuid = BLE_UUID16_DECLARE(0x2A24), .access_cb = gatt_access,
              .flags = BLE_GATT_CHR_F_READ },   /* Model Number */
            { .uuid = BLE_UUID16_DECLARE(0x2A26), .access_cb = gatt_access,
              .flags = BLE_GATT_CHR_F_READ },   /* Firmware Revision */
            { 0 }
        },
    },
    /* SmartFarm 业务服务 */
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &uuid_svc.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            { .uuid = &uuid_telemetry.u, .access_cb = gatt_access,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY },
            { .uuid = &uuid_command.u, .access_cb = gatt_access,
              /* WRITE_ENC：未加密的链路**写不进来**，由协议栈强制拒绝，
               * 不依赖应用层自觉检查。这是 A8 的核心。 */
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC |
                       BLE_GATT_CHR_F_NOTIFY },
            { .uuid = &uuid_status.u, .access_cb = gatt_access,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY },
            { .uuid = &uuid_wifi.u, .access_cb = gatt_access,
              /* 配网同理：否则旁人可读走/改写 WiFi 密码 */
              .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC |
                       BLE_GATT_CHR_F_NOTIFY },
            { 0 }
        },
    },
    { 0 }
};

/* ---------------- 特征读写 ---------------- */

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    const ble_uuid_t *u = ctxt->chr->uuid;
    int rc;

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        const char *value = NULL;
        uint16_t u16 = ble_uuid_u16(u);   /* 128 位 UUID 时返回 0 */

        if (u16 == 0x2A29)                              value = "SmartFarm Project";
        else if (u16 == 0x2A24)                         value = "SF-ESP32-D0WD-V3";
        else if (u16 == 0x2A26)                         value = SMARTFARM_FW_VERSION;
        else if (ble_uuid_cmp(u, &uuid_telemetry.u) == 0) value = s_telemetry;
        else if (ble_uuid_cmp(u, &uuid_status.u) == 0)    value = s_status;

        if (value == NULL) {
            ESP_LOGW(TAG, "read of unknown characteristic");
            return BLE_ATT_ERR_UNLIKELY;
        }
        rc = os_mbuf_append(ctxt->om, value, strlen(value));
        return (rc == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        /* 注意：om 不一定以 '\0' 结尾，必须按长度取 */
        uint8_t buf[192];
        uint16_t len = 0;
        rc = ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf) - 1, &len);
        if (rc != 0) return BLE_ATT_ERR_UNLIKELY;
        buf[len] = '\0';

        if (ble_uuid_cmp(u, &uuid_command.u) == 0) {
            /* 指令：解析执行后把回执从**同一特征**推回（Write + Notify 配对使用）。
             * ⚠️ 日志必须走 cmd_mask_for_log：本特征 allow_provisioning=true，
             * 往**这里**写 wifi:pass:<密码> 同样合法，原文打印会让密码进串口日志
             * （下面 wifi 特征那条路径只打长度，两条路径的防护必须一致）。 */
            cmd_status_t st = cmd_execute((const char *)buf, s_ack, sizeof(s_ack), true);
            char shown[96];
            cmd_mask_for_log((const char *)buf, shown, sizeof(shown));
            ESP_LOGI(TAG, "cmd \"%s\" -> %s", shown, s_ack);

            /* 回执推不出去时给出明确原因：绝大多数情况是写指令前没在该特征上开启通知 */
            esp_err_t ar = ble_svc_notify_command(s_ack);
            if (ar != ESP_OK) {
                ESP_LOGW(TAG, "ACK not pushed (%s) - enable NOTIFY on the command"
                              " characteristic before writing", esp_err_to_name(ar));
            }
            if (st == CMD_OK_PUSH_TELEM) {
                ble_svc_notify_telemetry(NULL);   /* 推最新那份（主循环每秒更新） */
            }
        } else if (ble_uuid_cmp(u, &uuid_wifi.u) == 0) {
            /* 配网：复用同一个解析器。
             * ⚠️ **绝不打印原始内容**——里面可能含 WiFi 密码。
             * 只打印长度与回执，而回执本身也只有长度、不含密码。 */
            cmd_execute((const char *)buf, s_ack, sizeof(s_ack), true);
            ESP_LOGI(TAG, "wifi cmd (%u bytes) -> %s", (unsigned)len, s_ack);

            esp_err_t ar = ble_svc_notify_wifi(s_ack);
            if (ar != ESP_OK) {
                ESP_LOGW(TAG, "ACK not pushed (%s) - enable NOTIFY on the wifi"
                              " characteristic before writing", esp_err_to_name(ar));
            }
        } else {
            ESP_LOGW(TAG, "write to non-writable characteristic");
            return BLE_ATT_ERR_UNLIKELY;
        }
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

/* ---------------- 广播与连接事件 ---------------- */

static void advertise(void)
{
    struct ble_hs_adv_fields fields = { 0 };
    struct ble_gap_adv_params adv_params = { 0 };
    int rc;

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)s_dev_name;
    fields.name_len = strlen(s_dev_name);
    fields.name_is_complete = 1;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields failed: %d", rc);
        return;
    }

    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;   /* 可连接 */
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;   /* 可被发现 */

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start failed: %d", rc);
        return;
    }
    ESP_LOGI(TAG, "advertising as \"%s\"", s_dev_name);
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "central CONNECTED (conn_handle=%d)", s_conn_handle);
        } else {
            ESP_LOGW(TAG, "connect failed: status=%d", event->connect.status);
            advertise();   /* 连接失败要重新开始广播 */
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "central DISCONNECTED: reason=0x%02x", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        /* 三个 notify 标志都要清干净：只清遥测那个的话，重连后若对端
         * 没有重新订阅，代码仍以为可以推送，notify 会静默失败并刷告警日志。 */
        s_notify_enabled = false;
        s_cmd_notify_enabled = false;
        s_wifi_notify_enabled = false;
        s_mtu = 23;          /* 回到规范默认值，等下次连接重新协商 */
        s_mtu_warned = false;
        advertise();         /* 断开后重新广播，等待下次连接 */
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;

    case BLE_GAP_EVENT_MTU:
        s_mtu = event->mtu.value;
        ESP_LOGI(TAG, "MTU negotiated: %u (single notify payload max %u bytes)",
                 s_mtu, (unsigned)(s_mtu - 3));
        break;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        /* 配对时由本机显示 6 位 Passkey，用户在手机上输入。
         * 要求输入即"认证配对"，可防中间人——比 Just Works 强。 */
        if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
            struct ble_sm_io pkey = { 0 };
            pkey.action = BLE_SM_IOACT_DISP;
            pkey.passkey = SECRET_BLE_PASSKEY;
            int rc2 = ble_sm_inject_io(event->passkey.conn_handle, &pkey);
            /* ⚠️ 这里**不打印 passkey 值**（连 DEBUG 级也不打）：
             * 凭据一律不进日志，见 README 第 8 条坑。
             * 值本身是操作者自己在 secrets.h 里设的，不需要从日志里读。 */
            ESP_LOGW(TAG, "PAIRING: enter your 6-digit passkey on the phone"
                          " (the value you set as SECRET_BLE_PASSKEY in main/secrets.h)"
                          "  rc=%d", rc2);
        } else {
            ESP_LOGW(TAG, "pairing: unsupported IO action %d", event->passkey.params.action);
        }
        break;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "ENCRYPTION %s (status=%d)",
                 event->enc_change.status == 0 ? "ENABLED" : "not established",
                 event->enc_change.status);
        break;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* 对端拿着旧密钥来重新配对（如手机端删过绑定）。
         * 删掉旧绑定并重配，否则会一直配不上。 */
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
            ESP_LOGW(TAG, "repeat pairing: dropped old bond, retrying");
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe: attr_handle=%d notify=%d indicate=%d",
                 event->subscribe.attr_handle, event->subscribe.cur_notify,
                 event->subscribe.cur_indicate);
        if (event->subscribe.attr_handle == s_telemetry_val_handle) {
            s_notify_enabled = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "telemetry notify %s",
                     s_notify_enabled ? "ENABLED" : "disabled");
        } else if (event->subscribe.attr_handle == s_cmd_val_handle) {
            s_cmd_notify_enabled = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "command notify %s (ACK will be pushed here)",
                     s_cmd_notify_enabled ? "ENABLED" : "disabled");
        } else if (event->subscribe.attr_handle == s_wifi_val_handle) {
            s_wifi_notify_enabled = event->subscribe.cur_notify;
            ESP_LOGI(TAG, "wifi notify %s (provisioning progress will be pushed here)",
                     s_wifi_notify_enabled ? "ENABLED" : "disabled");
        }
        break;

    default:
        break;
    }
    return 0;
}

/* ---------------- 主机生命周期 ---------------- */

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "nimble host reset, reason=%d", reason);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_util_ensure_addr failed: %d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed: %d", rc);
        return;
    }

    /* 取特征的值句柄：订阅事件只给 attr_handle，靠它判断订阅的是哪个特征 */
    rc = ble_gatts_find_chr(&uuid_svc.u, &uuid_telemetry.u, NULL, &s_telemetry_val_handle);
    if (rc != 0) {
        ESP_LOGW(TAG, "find_chr(telemetry) failed: %d", rc);
    }
    rc = ble_gatts_find_chr(&uuid_svc.u, &uuid_command.u, NULL, &s_cmd_val_handle);
    if (rc != 0) {
        ESP_LOGW(TAG, "find_chr(command) failed: %d", rc);
    }
    rc = ble_gatts_find_chr(&uuid_svc.u, &uuid_wifi.u, NULL, &s_wifi_val_handle);
    if (rc != 0) {
        ESP_LOGW(TAG, "find_chr(wifi) failed: %d", rc);
    }

    advertise();
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();            /* 阻塞直到 nimble_port_stop() */
    nimble_port_freertos_deinit();
}

/* ---------------- 对外接口 ---------------- */

esp_err_t ble_svc_start(void)
{
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_dev_name, sizeof(s_dev_name), "SmartFarm-%02X%02X", mac[4], mac[5]);

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* ---- A8：BLE 安全 ----
     * 目标：同一蓝牙范围内的陌生人**不能**操作继电器、也拿不到 WiFi 密码。
     * 三条防线：
     *   1. **认证配对**（本机显示 6 位 Passkey，手机输入）→ 防中间人，
     *      比 Just Works 强：Just Works 虽加密但攻击者可在配对瞬间冒充设备。
     *   2. **绑定**（bonding）→ 密钥存 NVS，已配对的手机以后免输。
     *   3. **特征标记 WRITE_ENC** → 未加密链路写不进来，由协议栈强制拒绝。
     *
     * 遥测特征**保持开放**：它只是传感器数据，不配对也能看，
     * 这样"靠近就能看数据"的体验不受影响，而控制与凭据必须加密。 */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;   /* 本机显示 Passkey */
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;                           /* 要求认证配对 */
    ble_hs_cfg.sm_sc = 1;                             /* LE Secure Connections */
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    /* bond 存满时的淘汰策略：轮转删除最旧的绑定。
     * 不设它时 ble_store 在满时只会回 ENOTSUP，于是 MAX_BONDS(3) 用满之后
     * **第 4 台手机永远配不上**，而且不会自动清理——表现为"换了手机就配不上，
     * 只能擦 NVS 重来"。设了它就能自动腾位。 */
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* 启动时明确告知 Passkey 的**安全状态**（但**不打印值本身**）。
     * 仍在用仓库里的公开默认值时必须显眼告警：知道该默认值的人
     * 在同一蓝牙范围内就能配对并操作继电器，A8 等于没做。 */
    if (SECRET_BLE_PASSKEY == SMARTFARM_BLE_PASSKEY_DEFAULT) {
        ESP_LOGE(TAG, "BLE passkey is the PUBLIC DEFAULT (%d) - anyone who knows it can"
                      " pair and control the relays. Set SECRET_BLE_PASSKEY in"
                      " main/secrets.h before connecting real loads!",
                 SMARTFARM_BLE_PASSKEY_DEFAULT);
    } else {
        ESP_LOGI(TAG, "BLE passkey: custom value configured (not logged)");
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(s_dev_name);

    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
        return ESP_FAIL;
    }

    /* 把 WiFi 配网状态接到本模块，用于把"连接中/成功/失败原因"推回手机。
     * 方向 wifi_mgr -> ble_svc，无循环依赖。 */
    wifi_mgr_set_state_cb(on_wifi_state);

    /* 非阻塞：主机在自己的任务里跑，广播就绪后由 on_sync 触发 */
    nimble_port_freertos_init(host_task);

    ESP_LOGI(TAG, "BLE started (non-blocking), device name \"%s\"", s_dev_name);
    ESP_LOGI(TAG, "  service f0a00001-8b3c-4d5e-9f10-2a3b4c5d6e7f"
                  " (telemetry/command/status/wifi)");
    ESP_LOGI(TAG, "  security: pairing required for command+wifi writes;"
                  " telemetry stays open");
    return ESP_OK;
}

bool ble_svc_is_connected(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

uint16_t ble_svc_mtu(void)
{
    return s_mtu;
}

esp_err_t ble_svc_set_telemetry(const char *json)
{
    if (json == NULL) return ESP_ERR_INVALID_ARG;
    strlcpy(s_telemetry, json, sizeof(s_telemetry));
    return ESP_OK;
}

/* 分片发送一条 Notify。
 *
 * 单次 Notify 的载荷上限是 (MTU-3)：ATT 头占 3 字节。
 * 中心设备不发起 MTU 交换时 MTU 只有 23，可用载荷仅 20 字节，
 * 而遥测 JSON 约 110 字节 —— 直接发会被 ATT 层**静默截断**（实测就是这样）。
 * 这里按需分片，每片带 "n/t|" 头便于重组：
 *
 *   1/7|{"air_ok":1,"a
 *   2/7|ir_t":29.3,"ai
 *   ...
 *
 * 协商到大 MTU（我们的 APP 会请求 512）时只有 1 片，几乎无额外开销。 */
static esp_err_t notify_chunked(uint16_t val_handle, const char *data, size_t len)
{
    if (s_mtu <= 3) return ESP_ERR_INVALID_SIZE;

    const size_t max_payload = (size_t)(s_mtu - 3);
    const size_t hdr_len = 4;                       /* "n/t|" */
    if (max_payload <= hdr_len) return ESP_ERR_INVALID_SIZE;

    const size_t per = max_payload - hdr_len;
    const size_t total = (len + per - 1) / per;

    /* 头部用单个数字表示序号，故最多 9 片。遥测 JSON 有界（约 110 字节），
     * 留有余量；真的超出就明确报错，绝不静默截断。 */
    if (total > 9) {
        ESP_LOGE(TAG, "telemetry %u bytes needs %u chunks but max is 9; shorten it",
                 (unsigned)len, (unsigned)total);
        return ESP_ERR_INVALID_SIZE;
    }

    if (total > 1 && !s_mtu_warned) {
        s_mtu_warned = true;
        ESP_LOGW(TAG, "MTU is only %u -> splitting telemetry into %u notifications;"
                      " a client requesting a larger MTU (e.g. 512) gets it in one piece",
                 s_mtu, (unsigned)total);
    }

    char buf[560];
    for (size_t i = 0; i < total; i++) {
        size_t off = i * per;
        size_t n = len - off;
        if (n > per) n = per;

        int hl = snprintf(buf, sizeof(buf), "%u/%u|", (unsigned)(i + 1), (unsigned)total);
        memcpy(buf + hl, data + off, n);

        struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, (uint16_t)(hl + n));
        if (om == NULL) return ESP_ERR_NO_MEM;

        /* 成功时 ble_gatts_notify_custom 接管并释放 om */
        int rc = ble_gatts_notify_custom(s_conn_handle, val_handle, om);
        if (rc != 0) {
            ESP_LOGW(TAG, "notify chunk %u/%u failed: %d",
                     (unsigned)(i + 1), (unsigned)total, rc);
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

esp_err_t ble_svc_notify_telemetry(const char *json)
{
    if (json != NULL) {
        ble_svc_set_telemetry(json);
    }

    /* 未连接/未订阅属于正常状态（设备开机时必然如此），调用方无需报错 */
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || !s_notify_enabled) {
        return ESP_ERR_INVALID_STATE;
    }

    return notify_chunked(s_telemetry_val_handle, s_telemetry, strlen(s_telemetry));
}

esp_err_t ble_svc_notify_command(const char *json)
{
    if (json == NULL) return ESP_ERR_INVALID_ARG;

    /* 对端没订阅指令特征时无法回执，属正常情况 */
    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || !s_cmd_notify_enabled) {
        return ESP_ERR_INVALID_STATE;
    }

    return notify_chunked(s_cmd_val_handle, json, strlen(json));
}

esp_err_t ble_svc_notify_wifi(const char *json)
{
    if (json == NULL) return ESP_ERR_INVALID_ARG;

    if (s_conn_handle == BLE_HS_CONN_HANDLE_NONE || !s_wifi_notify_enabled) {
        return ESP_ERR_INVALID_STATE;
    }

    return notify_chunked(s_wifi_val_handle, json, strlen(json));
}

/* WiFi 状态变化回调（由 wifi_mgr 在拿到 IP / 连续失败 / 清除凭据时调用）。
 * 方向是 wifi_mgr -> ble_svc，不构成循环依赖。 */
static void on_wifi_state(const char *json)
{
    esp_err_t err = ble_svc_notify_wifi(json);
    if (err != ESP_OK) {
        /* 没人订阅时属正常；仍打印到串口，便于没有手机在场时排查配网 */
        ESP_LOGI(TAG, "wifi state (not pushed: %s): %s", esp_err_to_name(err), json);
    }
}
