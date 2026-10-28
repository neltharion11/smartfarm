/* WiFi STA 连接 + SNTP 对时实现。 */

#include "wifi_mgr.h"
#include "net_config.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

/* lwIP 的 BSD socket / DNS 接口（用于 NTP 域名解析诊断） */
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define TAG "wifi"

#define BIT_CONNECTED BIT0

static EventGroupHandle_t s_eg;
static bool s_started;          /* 协议栈已初始化 */
static bool s_wifi_started;     /* esp_wifi_start() 已调用 */
static bool s_sntp_started;
static bool s_time_synced;
static int  s_disc_count;
static char s_ip_str[16] = "0.0.0.0";

/* 运行时凭据：优先来自 NVS（蓝牙配网写入），NVS 为空时回落到 secrets.h。
 * ⚠️ s_pass **绝不能**出现在任何日志或回执里。 */
#define WIFI_SSID_MAX 33
#define WIFI_PASS_MAX 65
static char s_ssid[WIFI_SSID_MAX];
static char s_pass[WIFI_PASS_MAX];

/* 最近一次断线原因（把"连不上"翻译成人能看懂的话回给配网方） */
static char s_fail_reason[80];

/* 当前凭据来自哪里："nvs" = 蓝牙配网写入（已持久化）；"secrets.h" = 编译期内置兜底。
 * 通过 wifi:status 回给手机，这样**验证 A6 不需要读串口**。 */
static const char *s_creds_src = "none";

/* 运行时改过凭据（尚未落盘）。只有**真正连上**之后才写 NVS——
 * 否则一次输错密码就会把可用凭据覆盖掉，重启后再也连不上。 */
static bool s_creds_dirty;

/* 「最后一次验证可用」的凭据副本 —— 配网失败时的回退目标。
 *
 * 为什么必须有它（**实测踩到的真实故障**）：
 *   用错误密码配网后，RAM 里的凭据被改坏，设备会拿着错密码无限重试，
 *   **永远回不到那套本来能用的凭据**。结果是"配一次网把设备搞失联"——
 *   装在农场里就得跑现场救。
 *   NVS 里那套好凭据虽然没被覆盖（只在连上后才落盘），
 *   但运行时不会自己回去用它，所以必须显式回退。
 *
 * 回退策略：连续失败达到阈值、且当前凭据与"已验证可用"那套不同 ->
 *   换回去、重连，并推一条 {"wifi":"reverted"} 告诉配网方
 *   "你给的凭据不可用，已恢复原设置"（否则用户会以为配网成功了）。 */
static char s_good_ssid[WIFI_SSID_MAX];
static char s_good_pass[WIFI_PASS_MAX];
static const char *s_good_src = "none";
static bool s_have_good;

#define WIFI_REVERT_AFTER_FAILS 5

/* 待应用的凭据变更。置位后，等下一次 STA_DISCONNECTED 事件里才设置新配置并重连。
 * 为什么要这样绕：esp_wifi_set_config() 在"仍在连接中"时会返回 ESP_ERR_WIFI_STATE，
 * 而 esp_wifi_disconnect() 是异步的——先断开、立刻设配置/重连必然撞上状态机。
 * 用事件保证"配置"发生在驱动完全断开之后，就不需要猜延时。 */
static bool s_apply_pending;

/* 状态变化回调：由 ble_svc 注册，用于把配网进度推回手机 */
static wifi_state_cb_t s_state_cb;

/* ---------------- NVS 凭据 ---------------- */

#define NVS_NS     "smartfarm"
#define NVS_K_SSID "wifi_ssid"
#define NVS_K_PASS "wifi_pass"

static void creds_save_nvs(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return;
    }
    nvs_set_str(h, NVS_K_SSID, s_ssid);
    nvs_set_str(h, NVS_K_PASS, s_pass);
    err = nvs_commit(h);
    if (err != ESP_OK) ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(err));
    nvs_close(h);
    s_creds_src = "nvs";   /* 落盘后来源就是 NVS 了 */
    /* 只报长度，不报内容 */
    ESP_LOGI(TAG, "credentials saved to NVS (ssid len %u, pass len %u)",
             (unsigned)strlen(s_ssid), (unsigned)strlen(s_pass));
}

static bool creds_load_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;

    size_t n = sizeof(s_ssid);
    if (nvs_get_str(h, NVS_K_SSID, s_ssid, &n) != ESP_OK) { nvs_close(h); return false; }
    n = sizeof(s_pass);
    if (nvs_get_str(h, NVS_K_PASS, s_pass, &n) != ESP_OK) { nvs_close(h); return false; }
    nvs_close(h);
    return (s_ssid[0] != '\0');
}

static void creds_clear_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, NVS_K_SSID);
    nvs_erase_key(h, NVS_K_PASS);
    nvs_commit(h);
    nvs_close(h);
}

/* ---------------- 状态回报 ---------------- */

static void notify_state(const char *json)
{
    if (s_state_cb != NULL) s_state_cb(json);
}

/* 把 WiFi reason code 翻译成可读原因——不同原因对应完全不同的处置，
 * 笼统地报"连不上"会让排查绕远路。 */
static const char *reason_str(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:        return "AP not found (SSID wrong / out of range)";
    case WIFI_REASON_AUTH_EXPIRE:        return "auth expire (transient, will retry)";
    case WIFI_REASON_AUTH_FAIL:          return "auth failed (WRONG PASSWORD?)";
    case WIFI_REASON_ASSOC_FAIL:         return "association failed";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:  return "handshake timeout (WRONG PASSWORD?)";
    case WIFI_REASON_CONNECTION_FAIL:    return "connection failed";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4-way handshake timeout (WRONG PASSWORD?)";
    case WIFI_REASON_BEACON_TIMEOUT:     return "beacon timeout (AP lost)";
    default:                             return "see esp_wifi_types.h reason codes";
    }
}

/* 把当前 s_ssid/s_pass 写入驱动配置。 */
static esp_err_t set_config_from_creds(void)
{
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, s_ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, s_pass, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;   /* 至少 WPA2；纯开放网络可改 OPEN */

    /* ---- BSSID 择优：连"最强的那个"，而不是"先扫到的那个" ----
     * 默认 scan_method = WIFI_FAST_SCAN，含义是"扫到第一个同名 AP 就停"
     * （见 esp_wifi_types_generic.h 的注释）。多 BSSID 路由器（mesh / 双频同名）
     * 下就会连到较弱的那个——实测同一台设备 RSSI 在 -37 与 -86 之间跳，
     * 弱链路会直接拖累 MQTT 的可靠性。
     *
     * 改成全信道扫描 + 按信号排序：驱动会收集所有同名 AP，按 RSSI 择优连接。 */
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    /* 某个 BSSID 连不上时换下一个，而不是死磕它（需配合 ALL_CHANNEL_SCAN） */
    wc.sta.failure_retry_cnt = 3;

    return esp_wifi_set_config(WIFI_IF_STA, &wc);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base != WIFI_EVENT) return;

    switch (id) {
    case WIFI_EVENT_STA_START:
        ESP_LOGI(TAG, "station started, connecting to \"%s\" ...", s_ssid);
        esp_wifi_connect();
        break;

    case WIFI_EVENT_STA_DISCONNECTED: {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        xEventGroupClearBits(s_eg, BIT_CONNECTED);
        s_disc_count++;
        snprintf(s_fail_reason, sizeof(s_fail_reason), "%s", reason_str(d->reason));
        /* 固定错误只在前几次刷屏，避免密码错时日志淹掉其它信息 */
        if (s_disc_count <= 3 || (s_disc_count % 20) == 0) {
            ESP_LOGW(TAG, "disconnected #%d: reason=%d (%s)", s_disc_count, d->reason,
                     reason_str(d->reason));
        }
        /* 连续失败 3 次就明确回报失败，别让配网方一直干等 */
        if (s_disc_count == 3) {
            char buf[192];
            snprintf(buf, sizeof(buf), "{\"wifi\":\"fail\",\"reason\":\"%s\"}", s_fail_reason);
            notify_state(buf);
        }

        /* 此刻驱动已完全断开，是应用新凭据的安全时机（见 s_apply_pending 说明） */
        if (s_apply_pending) {
            s_apply_pending = false;
            esp_err_t err = set_config_from_creds();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "apply new credentials failed: %s", esp_err_to_name(err));
            } else {
                ESP_LOGI(TAG, "new credentials applied (ssid len %u), reconnecting",
                         (unsigned)strlen(s_ssid));
                s_disc_count = 0;   /* 新一轮尝试，重新计数 */
            }
        }

        /* 连续失败到阈值：回退到"最后一次验证可用"的凭据。
         * 这是**防失联**的关键——否则一次错误密码就能把设备永久搞下线。 */
        if (s_disc_count >= WIFI_REVERT_AFTER_FAILS && s_have_good &&
            (strcmp(s_ssid, s_good_ssid) != 0 || strcmp(s_pass, s_good_pass) != 0)) {
            ESP_LOGW(TAG, "provisioned credentials failed %d times -> reverting to last"
                          " known-good (from %s)", s_disc_count, s_good_src);
            strlcpy(s_ssid, s_good_ssid, sizeof(s_ssid));
            strlcpy(s_pass, s_good_pass, sizeof(s_pass));
            s_creds_src = s_good_src;
            s_creds_dirty = false;      /* 回退后没有待落盘的改动 */
            s_disc_count = 0;
            if (set_config_from_creds() == ESP_OK) {
                /* 必须告诉配网方：它给的凭据没用，设备已恢复原设置 */
                notify_state("{\"wifi\":\"reverted\",\"reason\":\"new credentials unusable,"
                             " restored previous settings\"}");
            }
        }

        esp_wifi_connect();   /* 交由驱动持续重连 */
        break;
    }
    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base != IP_EVENT || id != IP_EVENT_STA_GOT_IP) return;

    ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
    snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&e->ip_info.ip));
    ESP_LOGI(TAG, "connected, got IP %s (after %d disconnect(s))", s_ip_str, s_disc_count);
    s_disc_count = 0;
    s_fail_reason[0] = '\0';
    xEventGroupSetBits(s_eg, BIT_CONNECTED);

    /* 只有真正连上了才把运行时凭据落盘（见 s_creds_dirty 的说明） */
    if (s_creds_dirty) {
        s_creds_dirty = false;
        creds_save_nvs();
    }

    /* 记下这套"验证过能用"的凭据，作为以后配网失败时的回退目标 */
    strlcpy(s_good_ssid, s_ssid, sizeof(s_good_ssid));
    strlcpy(s_good_pass, s_pass, sizeof(s_good_pass));
    s_good_src = s_creds_src;
    s_have_good = true;

    {
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"wifi\":\"ok\",\"ip\":\"%s\",\"rssi\":%d}",
                 s_ip_str, wifi_mgr_rssi());
        notify_state(buf);
    }
}

/* 协议栈初始化（幂等）。**不需要凭据**，因此蓝牙配网时也能先把栈拉起来。 */
static esp_err_t stack_init(void)
{
    if (s_started) return ESP_OK;

    s_eg = xEventGroupCreate();
    if (!s_eg) return ESP_ERR_NO_MEM;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        on_ip_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    s_started = true;
    return ESP_OK;
}

/* 用当前 s_ssid/s_pass 配置并连接。
 *
 * ⚠️ 时序是本函数的全部难点，实测踩过 ESP_ERR_WIFI_STATE：
 *    - esp_wifi_set_config() 在**仍在连接中**时会失败（ESP-IDF 明确文档化）
 *    - esp_wifi_disconnect() 是**异步**的，紧接着调用 connect 也会失败
 *   因此"已经跑起来"这条路径**不在这里设配置、也不在这里连**，
 *   只发起断开并置位 s_apply_pending，由 STA_DISCONNECTED 事件完成后续。 */
static esp_err_t apply_and_connect(void)
{
    if (!s_wifi_started) {
        /* 首次：驱动空闲，可以按常规顺序来 */
        esp_err_t err = set_config_from_creds();
        if (err != ESP_OK) return err;
        err = esp_wifi_start();          /* 触发 WIFI_EVENT_STA_START -> connect */
        if (err == ESP_OK) s_wifi_started = true;
        return err;
    }

    s_disc_count = 0;
    s_apply_pending = true;

    esp_err_t derr = esp_wifi_disconnect();
    if (derr == ESP_ERR_WIFI_NOT_CONNECT) {
        /* 本来就没在连：不会产生 DISCONNECTED 事件，所以这里必须自己应用并连 */
        s_apply_pending = false;
        esp_err_t err = set_config_from_creds();
        if (err != ESP_OK) return err;
        return esp_wifi_connect();
    }
    if (derr != ESP_OK) {
        s_apply_pending = false;
        return derr;
    }
    /* 断开成功：STA_DISCONNECTED 处理器会设置新配置并重连 */
    return ESP_OK;
}

esp_err_t wifi_mgr_start(void)
{
    /* 凭据优先级：NVS（蓝牙配网写入） > secrets.h（编译期内置兜底） */
    if (creds_load_nvs()) {
        s_creds_src = "nvs";
        ESP_LOGI(TAG, "credentials from NVS (previously provisioned over BLE)");
    } else {
        strlcpy(s_ssid, SECRET_WIFI_SSID, sizeof(s_ssid));
        strlcpy(s_pass, SECRET_WIFI_PASSWORD, sizeof(s_pass));
        if (s_ssid[0] != '\0') {
            s_creds_src = "secrets.h";
            ESP_LOGI(TAG, "credentials from secrets.h (built-in fallback)");
        }
        /* 两边都没有时保持 "none"，表示尚未配网 */
    }

    /* 启动时读到的这套就是上次能用的（NVS 只在连上后才落盘），
     * 先当作回退目标——这样开机后配网失败也有得退。 */
    if (s_ssid[0] != '\0') {
        strlcpy(s_good_ssid, s_ssid, sizeof(s_good_ssid));
        strlcpy(s_good_pass, s_pass, sizeof(s_good_pass));
        s_good_src = s_creds_src;
        s_have_good = true;
    }

    /* 即使暂时没有凭据也要把协议栈拉起来：否则蓝牙配网写入的凭据无处可用，
     * 配网功能就永远不可能工作。这是 A5 能成立的前提。 */
    esp_err_t err = stack_init();
    if (err != ESP_OK) return err;

    if (s_ssid[0] == '\0') {
        ESP_LOGW(TAG, "no WiFi credentials yet - waiting for BLE provisioning");
        return ESP_ERR_INVALID_STATE;
    }

    err = apply_and_connect();
    if (err != ESP_OK) return err;

    /* 只打印 SSID 与**密码长度**，绝不打印密码内容：
     * 长度为 0 或异常小可以立刻暴露"凭据没填对"这类问题。 */
    ESP_LOGI(TAG, "credentials: ssid=\"%s\" (len %u), password len=%u",
             s_ssid, (unsigned)strlen(s_ssid), (unsigned)strlen(s_pass));
    ESP_LOGI(TAG, "WiFi started (non-blocking); connection proceeds in background");
    return ESP_OK;
}

int wifi_mgr_disconnect_count(void)
{
    return s_disc_count;
}

/* authmode -> 可读字符串，用于扫描诊断 */
static const char *authmode_str(wifi_auth_mode_t m)
{
    switch (m) {
    case WIFI_AUTH_OPEN:            return "OPEN (no password)";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA-PSK";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2-PSK";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2 mixed";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-Enterprise";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3-PSK (SAE)";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3 mixed";
    default:                        return "unknown/other";
    }
}

void wifi_mgr_diag_scan(void)
{
    /* ⚠️ 必须用**运行时**的 s_ssid，而不是编译期的 SECRET_WIFI_SSID。
     *
     * s_ssid 才是设备此刻真正在连的那个 SSID（可能来自 NVS 或 BLE 配网）；
     * 而 SECRET_WIFI_SSID 只是 secrets.h 里的编译期回退值。
     * 经 BLE 换过 AP 之后两者会不一致：用编译期常量会去扫一个**无关的网络**，
     * 然后报"AP 未找到"，把排查方向完全带偏。 */
    if (s_ssid[0] == '\0') {
        ESP_LOGW(TAG, "diag scan skipped: no SSID configured yet");
        return;
    }

    ESP_LOGW(TAG, "--- scanning 2.4GHz for \"%s\" (blocking) ---", s_ssid);

    wifi_scan_config_t sc = {
        .ssid = (uint8_t *)s_ssid,
        .show_hidden = true,
    };
    esp_err_t err = esp_wifi_scan_start(&sc, true);   /* true = 阻塞直到完成 */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan start failed: %s", esp_err_to_name(err));
        return;
    }

    uint16_t ap_num = 0;
    esp_wifi_scan_get_ap_num(&ap_num);
    if (ap_num == 0) {
        ESP_LOGE(TAG, "AP \"%s\" not found in scan.", s_ssid);
        ESP_LOGE(TAG, "  => SSID wrong, or the router exposes it only on 5 GHz");
        ESP_LOGE(TAG, "     (ESP32 supports 2.4 GHz only)");
        return;
    }

    wifi_ap_record_t *recs = calloc(ap_num, sizeof(wifi_ap_record_t));
    if (!recs) { ESP_LOGE(TAG, "no mem for scan results"); return; }
    if (esp_wifi_scan_get_ap_records(&ap_num, recs) == ESP_OK) {
        for (int i = 0; i < ap_num; i++) {
            ESP_LOGW(TAG, "  found: ssid=\"%s\" rssi=%d ch=%d security=%s",
                     (const char *)recs[i].ssid, recs[i].rssi, recs[i].primary,
                     authmode_str(recs[i].authmode));
        }
        /* 给出结论：帮助区分"密码错"还是"模式不匹配" */
        wifi_auth_mode_t m = recs[0].authmode;
        if (m == WIFI_AUTH_WPA3_PSK) {
            ESP_LOGE(TAG, "  => AP is WPA3-only; WPA2 handshake will always fail here");
        } else if (m == WIFI_AUTH_WPA2_WPA3_PSK) {
            ESP_LOGW(TAG, "  => WPA2/WPA3 mixed: should work via WPA2, but some routers"
                          " require PMF; if handshake keeps timing out, check router settings");
        } else if (m == WIFI_AUTH_WPA2_PSK || m == WIFI_AUTH_WPA_WPA2_PSK) {
            ESP_LOGE(TAG, "  => AP is plain WPA2 -> 4-way handshake timeout almost always"
                          " means the PASSWORD IS WRONG");
        }
    }
    free(recs);
}

bool wifi_mgr_is_connected(void)
{
    if (!s_eg) return false;
    return (xEventGroupGetBits(s_eg) & BIT_CONNECTED) != 0;
}

esp_err_t wifi_mgr_sntp_start(void)
{
    if (s_sntp_started) return ESP_OK;
    if (!wifi_mgr_is_connected()) return ESP_ERR_INVALID_STATE;

    /* 国内农场：设置时区便于日志可读（CST-8 表示 UTC+8，POSIX 的符号是反的） */
    setenv("TZ", "CST-8", 1);
    tzset();

    /* 先探测 DNS，把"DNS 解析不了"与"能解析但 NTP 被拦"这两类原因分开。
     * 二者都会表现为"时间一直不同步"，但处置完全不同。 */
    struct addrinfo hints = { 0 };
    hints.ai_family = AF_INET;
    struct addrinfo *res = NULL;
    int rc = getaddrinfo(SMARTFARM_NTP_SERVER, NULL, &hints, &res);
    if (rc == 0 && res != NULL) {
        char ip[32] = { 0 };
        struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
        inet_ntoa_r(sa->sin_addr, ip, sizeof(ip));
        ESP_LOGI(TAG, "DNS ok: %s -> %s", SMARTFARM_NTP_SERVER, ip);
        freeaddrinfo(res);
    } else {
        ESP_LOGE(TAG, "DNS FAILED for %s (rc=%d) -> no internet / DNS broken", SMARTFARM_NTP_SERVER, rc);
        ESP_LOGE(TAG, "  => SNTP will not work; TLS needs a correct clock, so MQTT cannot start");
        return ESP_ERR_INVALID_STATE;
    }

    /* 用国内可达的 NTP 服务器：pool.ntp.org 在国内经常超时 */
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(SMARTFARM_NTP_SERVER);
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SNTP init failed: %s", esp_err_to_name(err));
        return err;
    }
    s_sntp_started = true;
    ESP_LOGI(TAG, "SNTP started with %s (required before MQTT/TLS: cert validity is checked against clock)",
             SMARTFARM_NTP_SERVER);
    return ESP_OK;
}

bool wifi_mgr_time_synced(void)
{
    if (!s_sntp_started) return false;

    /* 只探测一次并把结果缓存。
     * 实测：esp_netif_sntp_sync_wait(0) 以 0 超时轮询不够稳定——
     * 同一毫秒内连续调用两次可能一次返回成功、一次返回失败，
     * 会导致主循环认为"已同步"而 mqtt_app_start() 认为"未同步"，
     * 从而永远启动不了 MQTT。缓存后行为确定。 */
    if (!s_time_synced) {
        s_time_synced = (esp_netif_sntp_sync_wait(0) == ESP_OK);
        if (s_time_synced) {
            time_t now = time(NULL);
            struct tm tm_now;
            localtime_r(&now, &tm_now);
            ESP_LOGI(TAG, "time synced: %04d-%02d-%02d %02d:%02d:%02d (CST-8)",
                     tm_now.tm_year + 1900, tm_now.tm_mon + 1, tm_now.tm_mday,
                     tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
        }
    }
    return s_time_synced;
}

const char *wifi_mgr_ip_str(void)
{
    return s_ip_str;
}

int wifi_mgr_rssi(void)
{
    if (!wifi_mgr_is_connected()) return 0;
    wifi_ap_record_t ap = { 0 };
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return 0;
    return ap.rssi;
}

/* ---------------- 配网接口（供 BLE 层调用） ---------------- */

/* SSID 里可能含引号或反斜杠，直接拼进 JSON 会产生非法 JSON 让 APP 解析失败 */
static void json_escape(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0' && o + 2 < n; i++) {
        if (in[i] == '"' || in[i] == '\\') out[o++] = '\\';
        out[o++] = in[i];
    }
    out[o] = '\0';
}

void wifi_mgr_set_state_cb(wifi_state_cb_t cb)
{
    s_state_cb = cb;
}

esp_err_t wifi_mgr_set_ssid(const char *ssid)
{
    if (ssid == NULL || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (strlen(ssid) >= sizeof(s_ssid)) return ESP_ERR_INVALID_SIZE;
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    s_creds_dirty = true;
    ESP_LOGI(TAG, "pending SSID set (len %u)", (unsigned)strlen(s_ssid));
    return ESP_OK;
}

esp_err_t wifi_mgr_set_pass(const char *pass)
{
    if (pass == NULL) return ESP_ERR_INVALID_ARG;
    if (strlen(pass) >= sizeof(s_pass)) return ESP_ERR_INVALID_SIZE;
    strlcpy(s_pass, pass, sizeof(s_pass));
    s_creds_dirty = true;
    /* ⚠️ 绝不打印密码内容 */
    ESP_LOGI(TAG, "pending password set (len %u)", (unsigned)strlen(s_pass));
    return ESP_OK;
}

esp_err_t wifi_mgr_connect_with_pending(void)
{
    if (s_ssid[0] == '\0') {
        ESP_LOGW(TAG, "cannot connect: SSID not set");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = stack_init();
    if (err != ESP_OK) return err;

    /* 注意：这里**不**落盘。凭据要等真正连上（拿到 IP）后才写 NVS，
     * 否则一次输错密码就会覆盖掉原本可用的凭据。见 on_ip_event()。 */

    {
        char esc[WIFI_SSID_MAX * 2];
        char buf[192];
        json_escape(s_ssid, esc, sizeof(esc));
        snprintf(buf, sizeof(buf), "{\"wifi\":\"connecting\",\"ssid\":\"%s\"}", esc);
        notify_state(buf);
    }

    ESP_LOGI(TAG, "connecting to \"%s\" ...", s_ssid);
    return apply_and_connect();
}

esp_err_t wifi_mgr_clear_credentials(void)
{
    creds_clear_nvs();
    s_ssid[0] = '\0';
    s_pass[0] = '\0';
    ESP_LOGI(TAG, "credentials cleared from NVS");
    notify_state("{\"wifi\":\"cleared\"}");
    return ESP_OK;
}

const char *wifi_mgr_ssid(void)
{
    return s_ssid;
}

esp_err_t wifi_mgr_state_json(char *out, size_t n)
{
    if (out == NULL || n == 0) return ESP_ERR_INVALID_ARG;

    char esc[WIFI_SSID_MAX * 2];
    json_escape(s_ssid, esc, sizeof(esc));

    snprintf(out, n,
             "{\"wifi\":\"status\",\"src\":\"%s\",\"ssid\":\"%s\",\"connected\":%d,"
             "\"ip\":\"%s\",\"rssi\":%d,\"disc\":%d,\"fail\":\"%s\"}",
             s_creds_src, esc, wifi_mgr_is_connected() ? 1 : 0, s_ip_str,
             wifi_mgr_rssi(), s_disc_count, s_fail_reason);
    return ESP_OK;
}
