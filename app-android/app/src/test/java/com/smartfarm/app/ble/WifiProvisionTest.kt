package com.smartfarm.app.ble

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 配网消息解析的单元测试。
 *
 * 这段逻辑的风险是**把失败读成成功**或反过来 —— 两者都不会抛异常，
 * 只会让用户看到错误的结论（"配网成功"其实失败了，最糟）。
 * 所以每种真实回执格式都钉一遍。
 *
 * 用例全部取自固件 `cmd.c` / `wifi_mgr.c` 的**真实输出格式**。
 */
class WifiProvisionTest {

    // ---------------------------------------------------------------- 指令回执

    @Test
    fun `带 ok=1 的成功回执`() {
        val ev = WifiProvision.parse("""{"ack":"wifi:connect","ok":1}""")
        assertTrue(ev is ProvEvent.Ack)
        ev as ProvEvent.Ack
        assertTrue(ev.ok)
        assertEquals("wifi:connect", ev.command)
    }

    @Test
    fun `拒绝回执没有 ok 字段_必须判为失败`() {
        // 固件真实输出：远程通道白名单拒绝
        val raw = """{"ack":"denied","err":"not allowed on remote channel",""" +
                """"hint":"WiFi settings can only be changed over Bluetooth"}"""
        val ev = WifiProvision.parse(raw)
        assertTrue(ev is ProvEvent.Ack)
        ev as ProvEvent.Ack
        assertFalse("没有 ok 但带 err 的回执必须判为失败", ev.ok)
        assertTrue(ev.detail.contains("not allowed"))
    }

    @Test
    fun `没有 ok 也没有 err 的回执视为成功`() {
        // 例如 {"ack":"get"} / {"ack":"wifi:clear"}
        val ev = WifiProvision.parse("""{"ack":"wifi:clear"}""") as ProvEvent.Ack
        assertTrue(ev.ok)
    }

    @Test
    fun `密码回执只含长度_显示文字里没有任何密码内容`() {
        val ev = WifiProvision.parse("""{"ack":"wifi:pass","ok":1,"len":13}""") as ProvEvent.Ack
        assertTrue(ev.ok)
        // 精确断言：全部信息就是"长度 13"。固件本来也只回长度，不回内容。
        assertEquals("长度 13", ev.detail)
        assertEquals("✓ wifi:pass  长度 13", ev.display())
    }

    // ---------------------------------------------------------------- 连接状态

    @Test
    fun `配网成功带出 IP 与信号`() {
        val ev = WifiProvision.parse("""{"wifi":"ok","ip":"192.168.2.40","rssi":-45}""")
        assertTrue(ev is ProvEvent.State)
        ev as ProvEvent.State
        assertEquals("ok", ev.state)
        assertTrue(ev.detail.contains("192.168.2.40"))
        assertTrue(ev.detail.contains("-45"))
        assertTrue(ev.display().contains("配网成功"))
    }

    @Test
    fun `配网失败要显示可读原因`() {
        val raw = """{"wifi":"fail","reason":"4-way handshake timeout (WRONG PASSWORD?)"}"""
        val ev = WifiProvision.parse(raw) as ProvEvent.State
        assertEquals("fail", ev.state)
        assertTrue("失败原因必须原样带出，否则用户无从排查", ev.detail.contains("WRONG PASSWORD"))
        assertTrue(ev.display().contains("配网失败"))
    }

    @Test
    fun `状态查询要能看出凭据来源`() {
        val raw = """{"wifi":"status","src":"nvs","ssid":"809","connected":1,""" +
                """"ip":"192.168.2.40","rssi":-45,"disc":0,"fail":""}"""
        val ev = WifiProvision.parse(raw) as ProvEvent.State
        assertEquals("status", ev.state)
        assertTrue(ev.detail.contains("809"))
        assertTrue("凭据来源 nvs 是判断'是否已配网持久化'的关键", ev.detail.contains("nvs"))
        assertTrue(ev.detail.contains("192.168.2.40"))
    }

    @Test
    fun `设备回退必须明确提示_不能让用户以为配网成功`() {
        // 设备侧行为：新凭据连续失败若干次后，自动恢复上一次可用设置并推这条消息
        val raw = """{"wifi":"reverted","reason":"new credentials unusable,""" +
                """ restored previous settings"}"""
        val ev = WifiProvision.parse(raw) as ProvEvent.State
        assertEquals("reverted", ev.state)
        val shown = ev.display()
        assertTrue("必须带警告字样", shown.contains("⚠"))
        assertTrue("必须说明已恢复原设置", shown.contains("恢复"))
    }

    @Test
    fun `连接中消息带出 SSID`() {
        val ev = WifiProvision.parse("""{"wifi":"connecting","ssid":"809"}""") as ProvEvent.State
        assertEquals("connecting", ev.state)
        assertTrue(ev.detail.contains("809"))
    }

    // ---------------------------------------------------------------- 非配网消息

    @Test
    fun `遥测 JSON 不是配网消息_归入 Unknown`() {
        val ev = WifiProvision.parse("""{"air_ok":1,"air_t":29.9}""")
        assertTrue(ev is ProvEvent.Unknown)
    }

    @Test
    fun `畸形 JSON 归入 Unknown 而不抛异常`() {
        assertTrue(WifiProvision.parse("garbage") is ProvEvent.Unknown)
        assertTrue(WifiProvision.parse("") is ProvEvent.Unknown)
    }
}
