#ifndef BLE_SVC_H
#define BLE_SVC_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/**
 * BLE GATT 外设（NimBLE）
 *
 * 角色：只做外设（手机连设备），不需要经典蓝牙，故用 NimBLE 而非 Bluedroid。
 *
 * 设计原则与网络层一致：**蓝牙是"增强"，不是"前提"**。
 * 蓝牙起不来时，采集、显示、继电器、WiFi/MQTT 必须照常工作。
 *
 * UUID 规划（自定义 128 位，统一前缀便于识别）：
 *   SmartFarm 服务       f0a00001-8b3c-4d5e-9f10-2a3b4c5d6e7f
 *     遥测  Read+Notify      ...0002
 *     指令  Write            ...0003
 *     状态  Read+Notify      ...0004
 *     WiFi 配网 Write+Notify ...0005
 *   另含标准设备信息服务 0x180A（厂商/型号/固件版本），便于蓝牙助手立刻验证。
 *
 * ⚠️ ATT MTU：默认 23 时有效载荷仅 20 字节，而遥测 JSON 约 110 字节。
 *    已在 sdkconfig 设 CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=512，
 *    连接时会做 MTU 交换；实际值以 "MTU negotiated" 日志为准。
 */

/** 启动 BLE 广播与 GATT 服务（非阻塞）。 */
esp_err_t ble_svc_start(void);

/** 当前是否有中心设备（手机）已连接。 */
bool ble_svc_is_connected(void);

/** 已协商的 ATT MTU（未连接时为 0）。 */
uint16_t ble_svc_mtu(void);

/** 记录最新遥测 JSON（供遥测特征的 Read 返回）。 */
esp_err_t ble_svc_set_telemetry(const char *json);

/** 通过 Notify 推送遥测。
 *
 *  json 非空时先记录再推送；传 NULL 表示推送已记录的那份。
 *  未连接或对端未开启 Notify 时返回 ESP_ERR_INVALID_STATE —— 这属于**正常情况**，
 *  调用方不应据此刷错误日志。
 *  载荷超过单次 Notify 上限时会自动分片（每片带 "n/t|" 头），**绝不静默截断**。 */
esp_err_t ble_svc_notify_telemetry(const char *json);

/** 通过 Notify 在**指令特征**上回执（ACK / 错误说明）。
 *  指令特征同时具备 Write 与 Notify：写入指令，回执从同一特征推回。 */
esp_err_t ble_svc_notify_command(const char *json);

/** 通过 Notify 在**配网特征**上推回配网进度与结果。
 *  连接状态由 wifi_mgr 通过回调驱动，此处只负责传输。 */
esp_err_t ble_svc_notify_wifi(const char *json);

#endif /* BLE_SVC_H */
