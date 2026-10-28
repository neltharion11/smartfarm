package com.smartfarm.app.ble

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 遥测分片重组与解析的单元测试。
 *
 * 为什么值得单独测：这段逻辑有两类**静默错误**——
 *   1. 分片拼错 → 得到半截或错位的 JSON，界面显示乱值却看不出哪里错了
 *   2. 把 `null` 读成 0 → 传感器故障时显示「0.0 ℃」而不是「无效」
 * 两者都不会抛异常，只会安静地显示错数据。所以在电脑上先钉死。
 *
 * 用例数据取自设备真实输出（MTU 未协商时确实会分成 7 片）。
 */
class TelemetryAssemblerTest {

    private val fullJson =
        """{"air_ok":1,"air_t":29.9,"air_rh":59.1,"water_ok":1,"water_t":30.1,""" +
        """"relay":0,"ain":0,"rssi":-84,"up_s":62}"""

    // ---------------------------------------------------------------- 单片

    @Test
    fun `无前缀的完整 JSON 直接返回`() {
        val a = TelemetryAssembler()
        assertEquals(fullJson, a.feed(fullJson))
        assertFalse(a.isFragmented)
    }

    // ---------------------------------------------------------------- 分片

    @Test
    fun `按序分片_只有最后一片才拼成完整 JSON`() {
        val a = TelemetryAssembler()
        // 设备在 MTU=23 时每片净载荷 16 字节，故约 7 片
        val chunks = chunk(fullJson, 16, 7)
        assertEquals(7, chunks.size)

        for (i in 0 until chunks.size - 1) {
            assertNull("第 ${i + 1} 片不该产出完整 JSON", a.feed(chunks[i]))
        }
        assertEquals(fullJson, a.feed(chunks.last()))
        assertTrue(a.isFragmented)
    }

    @Test
    fun `首片未到时其余分片被安全丢弃_不会拼出错误结果`() {
        // 这是**有意为之**的语义，不是缺陷：
        // BLE 的 ATT 通知在链路上是**有序**的，分片必然按序号到达。
        // 实现据此要求"序号 1 先到才开启一批"——好处是：一旦上一批出现丢片，
        // 新一批的第 1 片会清掉残留，绝不会把两批数据拼在一起。
        val a = TelemetryAssembler()
        val chunks = chunk(fullJson, 16, 7)

        // 先喂非首片：应全部被丢弃，不产出任何东西
        for (i in 1 until chunks.size) assertNull(a.feed(chunks[i]))

        // 随后正常从第 1 片开始，仍能正确拼出完整 JSON
        for (i in 0 until chunks.size - 1) assertNull(a.feed(chunks[i]))
        assertEquals(fullJson, a.feed(chunks.last()))
    }

    @Test
    fun `新一批分片开始会丢弃上一批的残留`() {
        val a = TelemetryAssembler()
        val chunks = chunk(fullJson, 16, 7)
        assertNull(a.feed(chunks[0]))          // 只喂了第 1 片，缺片
        assertNull(a.feed(chunks[1]))
        // 新一批从头开始（序号又是 1）—— 应清掉旧残留并最终成功
        assertNull(a.feed(chunks[0]))
        for (i in 1 until chunks.size) {
            val r = a.feed(chunks[i])
            if (i == chunks.size - 1) assertEquals(fullJson, r) else assertNull(r)
        }
    }

    @Test
    fun `片数不一致的片被丢弃而不是拼出错误结果`() {
        val a = TelemetryAssembler()
        assertNull(a.feed("1/5|aaa"))
        // 总数与上一片不符 → 丢弃
        assertNull(a.feed("2/9|bbb"))
    }

    @Test
    fun `序号越界或非法头部不算有效分片`() {
        val a = TelemetryAssembler()
        // 8/7 序号大于总数 → 视为无效分片，返回 null（且不会拼出东西）
        assertNull(a.feed("8/7|xxx"))
        assertNull(a.feed("0/3|xxx"))
        // "12/3|" 这类头部本身格式合法但 index > count
        assertNull(a.feed("12/3|xxx"))
    }

    // ---------------------------------------------------------------- 解析

    @Test
    fun `解析有效遥测`() {
        val t = Telemetry.parse(fullJson)
        assertNotNull(t)
        t!!
        assertTrue(t.airOk)
        assertEquals(29.9, t.airTempC!!, 0.001)
        assertEquals(59.1, t.airRh!!, 0.001)
        assertTrue(t.waterOk)
        assertEquals(30.1, t.waterTempC!!, 0.001)
        assertEquals(0, t.relayMask)
        assertEquals(-84, t.rssi)
        assertEquals(62L, t.uptimeSec)
    }

    @Test
    fun `读数无效时是 null 而不是 0`() {
        // 设备在 DS18B20 读取失败时发的真实内容
        val json = """{"air_ok":1,"air_t":29.9,"air_rh":59.1,"water_ok":0,""" +
                """"water_t":null,"relay":0,"ain":0,"rssi":-84,"up_s":62}"""
        val t = Telemetry.parse(json)
        assertNotNull(t)
        t!!
        assertFalse(t.waterOk)
        assertNull("无效读数必须解析成 null，绝不能变成 0.0", t.waterTempC)
    }

    @Test
    fun `负数温度解析正确`() {
        val json = """{"air_ok":1,"air_t":-5.2,"air_rh":80.0,"water_ok":1,""" +
                """"water_t":-12.3,"relay":0,"ain":0,"rssi":-70,"up_s":1}"""
        val t = Telemetry.parse(json)!!
        assertEquals(-5.2, t.airTempC!!, 0.001)
        assertEquals(-12.3, t.waterTempC!!, 0.001)
    }

    @Test
    fun `继电器与输入掩码解析正确`() {
        val json = """{"air_ok":1,"air_t":20.0,"air_rh":50.0,"water_ok":1,""" +
                """"water_t":20.0,"relay":5,"ain":2,"rssi":-60,"up_s":9}"""
        val t = Telemetry.parse(json)!!
        assertEquals(5, t.relayMask)   // 1 0 1
        assertEquals(2, t.inputMask)   // 0 1 0
    }

    @Test
    fun `畸形 JSON 返回 null 而不抛异常`() {
        assertNull(Telemetry.parse("not json at all"))
        assertNull(Telemetry.parse(""))
    }

    // ---------------------------------------------------------------- 工具

    /** 按设备的实际分片方式切分：每片净载荷 per 字节，前缀为 "n/total|"。 */
    private fun chunk(json: String, per: Int, total: Int): List<String> {
        require(json.length <= per * total)
        val out = ArrayList<String>(total)
        for (i in 0 until total) {
            val from = i * per
            val to = minOf(from + per, json.length)
            if (from >= json.length) break
            out.add("${out.size + 1}/$total|" + json.substring(from, to))
        }
        // 修正：如果实际片数少于预设，需要重写头部保证一致性
        val real = out.size
        return out.mapIndexed { idx, s ->
            val body = s.substringAfter('|')
            "${idx + 1}/$real|$body"
        }
    }
}
