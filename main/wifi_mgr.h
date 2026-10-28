#ifndef WIFI_MGR_H
#define WIFI_MGR_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/**
 * WiFi STA 连接 + SNTP 对时
 *
 * 设计要点：
 *   - **非阻塞**：start() 只负责初始化并发起连接，不等待结果，
 *     这样显示屏与传感器不受联网成败影响（断网照常工作）。
 *   - **分类诊断**：断线时打印 reason code 并翻译成可读原因
 *     （AP 找不到 / 密码错 / 握手超时 …），而不是笼统报"连不上"。
 *   - **SNTP 必须在 MQTT 之前成功**：TLS 会校验证书有效期，
 *     系统时间若还停在 1970 年，证书会被判为"尚未生效"而验证失败。
 */

/** 初始化 WiFi 并发起连接（非阻塞）。凭据取自 net_config.h。 */
esp_err_t wifi_mgr_start(void);

/** 是否已连上 AP 并取得 IP。 */
bool wifi_mgr_is_connected(void);

/** 启动 SNTP 对时（应在拿到 IP 之后调用一次）。 */
esp_err_t wifi_mgr_sntp_start(void);

/** 时间是否已同步（非阻塞检查）。 */
bool wifi_mgr_time_synced(void);

/** 当前 IP 字符串（未连接时返回 "0.0.0.0"）。 */
const char *wifi_mgr_ip_str(void);

/** 当前 AP 信号强度 dBm（未连接或查询失败返回 0）。
 *  APP 侧可用它辅助判断"离得近该走蓝牙、离得远走 MQTT"。 */
int wifi_mgr_rssi(void);

/** 累计断线次数（连上后清零）。用于判断"偶尔抖动"还是"根本连不上"。 */
int wifi_mgr_disconnect_count(void);

/** 主动扫描并打印目标 AP 的安全模式 / 信道 / 信号强度。
 *
 *  用途：区分"密码错误"与"安全模式不匹配(WPA3/PMF)"这两类失败——
 *  它们的症状都是 4 次握手超时，但处置办法完全不同。
 *  这是**阻塞式**扫描，只应在故障诊断时调用（不要放事件回调里）。
 *  注意：ESP32 只支持 2.4 GHz，若路由器把该 SSID 只开在 5 GHz 上就扫不到。 */
void wifi_mgr_diag_scan(void);

/* ---------------- 蓝牙配网接口 ---------------- */

/** 配网/连接状态变化回调，参数是一段 JSON（由 BLE 层推回手机）。
 *  可能的值：connecting / ok / fail / cleared。 */
typedef void (*wifi_state_cb_t)(const char *json);

void wifi_mgr_set_state_cb(wifi_state_cb_t cb);

/** 暂存 SSID（只写 RAM，尚未落盘）。 */
esp_err_t wifi_mgr_set_ssid(const char *ssid);

/** 暂存密码（只写 RAM，尚未落盘）。**不会出现在任何日志里**。 */
esp_err_t wifi_mgr_set_pass(const char *pass);

/** 用暂存的凭据连接：**先落盘 NVS**，再配置并重连。
 *  即使开机时没有凭据（协议栈未起）也能工作——这是配网能成立的前提。 */
esp_err_t wifi_mgr_connect_with_pending(void);

/** 清除 NVS 中的凭据（恢复"未配网"状态）。 */
esp_err_t wifi_mgr_clear_credentials(void);

/** 当前 SSID（未配置时为空串）。 */
const char *wifi_mgr_ssid(void);

/** 生成状态 JSON（含 SSID/IP/RSSI/断线次数/最近失败原因）。 */
esp_err_t wifi_mgr_state_json(char *out, size_t n);

#endif /* WIFI_MGR_H */
