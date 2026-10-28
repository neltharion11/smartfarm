package com.smartfarm.app.ble

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ProvisionProgressTest {
    private fun waiting() = ProvisionProgress().begin("FarmWiFi")

    @Test
    fun `下发成功回执不能触发进入控制页`() {
        val next = waiting().onEvent(WifiProvision.parse("""{"ack":"wifi:connect","ok":1}"""))
        assertTrue(next.waiting)
        assertFalse(next.succeeded)
    }

    @Test
    fun `设备获取 IP 后才能确认成功`() {
        val next = waiting().onEvent(WifiProvision.parse("""{"wifi":"ok","ip":"192.168.1.8","rssi":-42}"""))
        assertTrue(next.succeeded)
        assertFalse(next.waiting)
        assertTrue(next.message.contains("192.168.1.8"))
    }

    @Test
    fun `已配网设备查询连接状态可以进入控制页`() {
        val next = ProvisionProgress().onEvent(WifiProvision.parse("""{"wifi":"status","connected":1}"""))
        assertTrue(next.succeeded)
    }

    @Test
    fun `未连接或缺少连接标记不能当作成功`() {
        for (raw in listOf("""{"wifi":"status","connected":0}""", """{"wifi":"status"}""")) {
            assertFalse(ProvisionProgress().onEvent(WifiProvision.parse(raw)).succeeded)
        }
    }

    @Test
    fun `写入拒绝和连接失败保留失败原因`() {
        for (raw in listOf(
            """{"ack":"wifi:connect","ok":0,"err":"no ssid"}""",
            """{"wifi":"fail","reason":"WRONG PASSWORD"}""",
        )) {
            val next = waiting().onEvent(WifiProvision.parse(raw))
            assertEquals(ProvisionProgress.Phase.FAILED, next.phase)
            assertFalse(next.succeeded)
            assertFalse(next.waiting)
            assertTrue(next.message.contains("no ssid") || next.message.contains("WRONG PASSWORD"))
        }
    }

    @Test
    fun `恢复旧网络后不能把新凭据配网显示为成功`() {
        val reverted = waiting().onEvent(WifiProvision.parse("""{"wifi":"reverted","reason":"restored previous settings"}"""))
        val next = reverted.onEvent(WifiProvision.parse("""{"wifi":"ok","ip":"192.168.1.9"}"""))
        assertEquals(ProvisionProgress.Phase.REVERTED, next.phase)
        assertFalse(next.succeeded)
        // 用户明确发起新一次配网后，正常成功事件可以结束等待。
        assertTrue(next.begin("NewWiFi").onEvent(WifiProvision.parse("""{"wifi":"ok"}""")).succeeded)
    }

    @Test
    fun `缺少最终通知会结束等待并允许查询重试`() {
        val next = waiting().timeout()
        assertFalse(next.waiting)
        assertFalse(next.succeeded)
        assertTrue(next.message.contains("查询状态"))
        assertTrue(next.begin("FarmWiFi").waiting)
    }

    @Test
    fun `成功后超时回调不会覆盖结果`() {
        val success = waiting().onEvent(WifiProvision.parse("""{"wifi":"ok"}"""))
        assertEquals(success, success.timeout())
    }
}
