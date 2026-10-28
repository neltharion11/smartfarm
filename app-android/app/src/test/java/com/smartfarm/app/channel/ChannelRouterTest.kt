package com.smartfarm.app.channel

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 通道路由策略（C5）。
 *
 * 这条策略是 PLAN C5 明文规定的（"依据 BLE 连接状态，不要用 RSSI 抖动判断"），
 * 选错会造成"指令发出去了却换了路"这种极难排查的现象，所以钉住。
 */
class ChannelRouterTest {

    @Test
    fun bluetoothWinsWhenReady() {
        // 近距离优先：即使 MQTT 也连着，也应走蓝牙
        val d = ChannelRouter.choose(bleReady = true, mqttConnected = true)
        assertEquals(ChannelRouter.Channel.BLE, d.channel)
        assertTrue(d.usable)
    }

    @Test
    fun bluetoothAloneIsEnough() {
        val d = ChannelRouter.choose(bleReady = true, mqttConnected = false)
        assertEquals(ChannelRouter.Channel.BLE, d.channel)
    }

    @Test
    fun fallsBackToMqttWhenBluetoothNotReady() {
        // "已连接但 CCCD 没写完"也必须落到 MQTT：
        // 否则指令发出去没有回执，用户只会等到一个超时
        val d = ChannelRouter.choose(bleReady = false, mqttConnected = true)
        assertEquals(ChannelRouter.Channel.MQTT, d.channel)
        assertTrue(d.reason.isNotEmpty())
    }

    @Test
    fun neitherAvailableIsExplicitlyUnusable() {
        val d = ChannelRouter.choose(bleReady = false, mqttConnected = false)
        assertEquals(ChannelRouter.Channel.NONE, d.channel)
        assertFalse("不可用必须能被调用方识别", d.usable)
        assertTrue("必须给出可操作的原因", d.reason.contains("靠近"))
    }

    @Test
    fun decisionIsStateless() {
        // 无状态：连续调用同一输入必须给同一结论。
        // 若将来有人加了"上次选了哪条"的隐藏状态，这条会失败。
        val first = ChannelRouter.choose(bleReady = false, mqttConnected = true)
        val second = ChannelRouter.choose(bleReady = false, mqttConnected = true)
        assertEquals(first, second)
    }
}
