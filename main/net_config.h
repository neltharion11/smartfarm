#ifndef NET_CONFIG_H
#define NET_CONFIG_H

/**
 * 网络配置与凭据入口
 *
 * 凭据放在本地 secrets.h（已 gitignore，见 secrets.h.example 模板）。
 * 仓库里**只有模板没有 secrets.h**，所以别人克隆后必须先创建它。
 * 这里用 __has_include 做优雅降级：没有 secrets.h 时回落到空字符串，
 * 保证工程**仍然可以编译**，只是连不上网络（并在日志里明确提示原因）。
 */

#if defined(__has_include)
#  if __has_include("secrets.h")
#    include "secrets.h"
#    define SMARTFARM_SECRETS_PRESENT 1
#  endif
#endif

#ifndef SMARTFARM_SECRETS_PRESENT
#  define SMARTFARM_SECRETS_PRESENT 0
#  define SECRET_WIFI_SSID     ""
#  define SECRET_WIFI_PASSWORD ""
#  define SECRET_MQTT_URI      ""
#  define SECRET_MQTT_USERNAME ""
#  define SECRET_MQTT_PASSWORD ""
#endif

/* 主题与节点标识。多个设备部署时改 NODE_ID 区分。 */
#define SMARTFARM_TOPIC_PREFIX "farm"
#define SMARTFARM_NODE_ID      "node1"

/* 运行时真正使用的主题（拼接后） */
#define SMARTFARM_TOPIC_STATUS    SMARTFARM_TOPIC_PREFIX "/" SMARTFARM_NODE_ID "/status"
#define SMARTFARM_TOPIC_TELEMETRY SMARTFARM_TOPIC_PREFIX "/" SMARTFARM_NODE_ID "/telemetry"
#define SMARTFARM_TOPIC_CMD       SMARTFARM_TOPIC_PREFIX "/" SMARTFARM_NODE_ID "/cmd"
/* 指令回执：与 BLE 侧的回执语义一致，APP 两条通道可以用同一套解析逻辑 */
#define SMARTFARM_TOPIC_ACK       SMARTFARM_TOPIC_PREFIX "/" SMARTFARM_NODE_ID "/ack"

/* 采集/上报间隔（毫秒） */
#define SMARTFARM_SAMPLE_INTERVAL_MS 1000

/* NTP 服务器：必须选国内可达的。
 * 实测 pool.ntp.org 在国内常超时，会导致 SNTP 一直不同步，
 * 进而使 MQTT 无法启动（TLS 要校验证书有效期，时钟不对就握手失败）。 */
#define SMARTFARM_NTP_SERVER "ntp.aliyun.com"

/* 固件版本：同时用于 BLE 设备信息服务的"固件版本"特征 */
#define SMARTFARM_FW_VERSION "0.1.0"

/* ---------------- BLE 配对 Passkey ---------------- */

/* BLE 配对时显示的 6 位数字 Passkey。
 *
 * 为什么需要它：BLE 的"Just Works"配对虽然会加密链路，但**不防中间人**——
 * 攻击者可以在配对瞬间冒充设备。要求输入 Passkey 才能得到认证配对（MITM 保护），
 * 这是"同一蓝牙范围内的陌生人不能操作继电器"的关键。
 *
 * ⚠️ 下面是**出厂默认值**，它写在开源仓库里 = 公开的。
 *    请在 secrets.h 里用 SECRET_BLE_PASSKEY 覆盖成你自己的随机 6 位数
 *    （secrets.h 已 gitignore，不会进仓库）。
 *
 * 固件启动时会检查这里：仍在用默认值时打**告警**并明确提示去改
 * （见 ble_svc.c 的 "BLE passkey is the PUBLIC DEFAULT"）；
 * 已自定义时只提示"已配置"，**不打印值本身**——凭据不进日志。 */
#ifndef SECRET_BLE_PASSKEY
#define SECRET_BLE_PASSKEY 123456
#endif

/* 供 ble_svc.c 判断"是否仍在用公开默认值"。改动默认值时这里要同步。 */
#define SMARTFARM_BLE_PASSKEY_DEFAULT 123456

#endif /* NET_CONFIG_H */
