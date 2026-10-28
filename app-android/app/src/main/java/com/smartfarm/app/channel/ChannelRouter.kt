package com.smartfarm.app.channel

/**
 * 通道路由（PLAN C5）。
 *
 * 决策依据是 **BLE 连接状态**，不是 RSSI：
 * PLAN 明确要求"依据 BLE 连接状态，**不要用 RSSI 抖动判断**"。
 * RSSI 会在阈值附近来回跳，一旦拿它决策就会出现通道频繁切换——
 * 指令发出去了却换了路、回执对不上号，比"选错一条"更难排查。
 *
 * 本对象是**纯函数、无状态**：同样输入永远给同样结论，
 * 因此不存在"上一次的选择影响这一次"的隐藏状态。
 */
object ChannelRouter {

    enum class Channel { BLE, MQTT, NONE }

    data class Decision(val channel: Channel, val reason: String) {
        val usable: Boolean get() = channel != Channel.NONE
    }

    /**
     * @param bleReady 蓝牙**已连接且已就绪**（CCCD 全部写完，能收遥测与回执）。
     *   仅"已连接"不算就绪：CCCD 没写完时写指令会没有回执，
     *   那种情况下应当先走 MQTT，而不是让用户对着一个不会回来的回执等超时。
     * @param mqttConnected broker 已连上
     */
    fun choose(bleReady: Boolean, mqttConnected: Boolean): Decision = when {
        // 近距离优先：蓝牙是本地链路，不耗服务器流量、延迟低
        bleReady -> Decision(Channel.BLE, "蓝牙已就绪（近距离优先）")
        mqttConnected -> Decision(Channel.MQTT, "蓝牙未就绪（未连接或未完成订阅），走 MQTT 远程")
        else -> Decision(
            Channel.NONE,
            "两条通道都不可用：蓝牙未就绪，MQTT 也未连接。" +
                    "请靠近设备用蓝牙，或先连接 MQTT。",
        )
    }
}
