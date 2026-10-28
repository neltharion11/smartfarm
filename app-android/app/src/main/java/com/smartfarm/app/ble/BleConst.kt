package com.smartfarm.app.ble

import java.util.UUID

/**
 * BLE 协议常量 —— 必须与固件 `main/ble_svc.c` 完全一致。
 *
 * 固件里的 UUID 用宏 `SMARTFARM_UUID128(n)` 生成，末尾 4 字节承载特征编号：
 *   人可读  f0a0000n-8b3c-4d5e-9f10-2a3b4c5d6e7f
 */
object BleConst {

    /** 设备名前缀。固件用 "SmartFarm-" + MAC 后 4 位十六进制（如 SmartFarm-DAA8）。 */
    const val DEVICE_NAME_PREFIX = "SmartFarm-"

    /** 自定义服务 */
    val SERVICE: UUID = UUID.fromString("f0a00001-8b3c-4d5e-9f10-2a3b4c5d6e7f")

    /** 遥测：Read + Notify。设备每秒推送一次 JSON。**不需要配对即可读取。** */
    val CHR_TELEMETRY: UUID = UUID.fromString("f0a00002-8b3c-4d5e-9f10-2a3b4c5d6e7f")

    /** 指令：Write + Notify。**需要加密链路（配对后）才能写入。** */
    val CHR_COMMAND: UUID = UUID.fromString("f0a00003-8b3c-4d5e-9f10-2a3b4c5d6e7f")

    /** 状态：Read + Notify。 */
    val CHR_STATUS: UUID = UUID.fromString("f0a00004-8b3c-4d5e-9f10-2a3b4c5d6e7f")

    /** WiFi 配网：Write + Notify。**需要加密链路。** */
    val CHR_WIFI: UUID = UUID.fromString("f0a00005-8b3c-4d5e-9f10-2a3b4c5d6e7f")

    /** 标准 CCCD（客户端特征配置描述符），开启通知用。 */
    val CCCD: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")

    /**
     * 请求的 ATT MTU。
     *
     * ⚠️ 这是**必须做对**的一件事：BLE 默认 ATT MTU 是 23，单次通知的有效载荷
     * 只有 20 字节，而遥测 JSON 约 110 字节 —— 不协商就会被截断成
     * `{"air_ok":1,"air_t":`（正好 20 字节）这种半截内容。
     * 设备侧已把首选 MTU 设为 512（ATT 上限）。
     */
    const val REQUEST_MTU = 512
}
