#ifndef MQTT_APP_H
#define MQTT_APP_H

#include <stdbool.h>
#include "esp_err.h"

/**
 * MQTT over TLS 客户端（对接 HiveMQ Cloud）
 *
 * 要点：
 *   - **必须 TLS**：HiveMQ Cloud 不提供明文入口，只有 8883(MQTTs) 与 8884(WSS)。
 *   - **证书验证用内置 CA 包**（esp_crt_bundle），不硬编码某张证书；
 *     代价是固件增加约 100 KB，好处是 CA 轮换无需重烧设备。
 *   - **SNTP 必须先成功**：TLS 会校验证书有效期，时钟停在 1970 会导致
 *     "证书尚未生效"而握手失败。因此 start() 会检查时间是否已同步。
 *   - **分类诊断**：把错误分成传输层(TLS/网络)、broker 拒绝(账号/授权) 两类，
 *     并给出可操作的结论，而不是笼统报"MQTT 失败"。
 *   - **不依赖 retained**：设备每次连上后主动发一次状态；retained 只是优化。
 */

/** 启动 MQTT 客户端（非阻塞，连接结果由事件回调反映）。
 *  需要 WiFi 已连接且时间已同步，否则返回 ESP_ERR_INVALID_STATE。 */
esp_err_t mqtt_app_start(void);

/** 是否已连上 broker。 */
bool mqtt_app_is_connected(void);

/** 发布一条消息。topic/payload 均需以 '\0' 结尾。 */
esp_err_t mqtt_app_publish(const char *topic, const char *payload, int qos, int retain);

/** 发布遥测（JSON）到 node 的 telemetry 主题。 */
esp_err_t mqtt_app_publish_telemetry(const char *json);

/** 取出并清除"立即上报"请求。
 *
 *  收到控制指令后本模块会置位该请求，主循环看到就立刻发一条遥测——
 *  否则 APP 要等 30 秒的下一个上报周期才能看到控制结果。
 *  组包需要传感器数据，所以由主循环做，本模块只负责置位。 */
bool mqtt_app_take_publish_request(void);

/** 目标板上自检**入站指令重组**（纯逻辑：不联网、不订阅、不发布任何消息）。
 *
 *  盯住的安全不变量：**超长或分片不一致的远端载荷绝不产生可执行指令**。
 *  cmd 主题对任何持有 MQTT 凭据的客户端开放，若超长载荷被截断后执行，
 *  可能生成与发送方意图完全不同的规则。详见 mqtt_app.c 的 mqtt_rx_feed。 */
void mqtt_app_selftest(void);

#endif /* MQTT_APP_H */
