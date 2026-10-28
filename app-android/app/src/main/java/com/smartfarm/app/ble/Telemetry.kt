package com.smartfarm.app.ble

import org.json.JSONObject

/**
 * 遥测数据。
 *
 * 与固件 `main/main.c` 的 `build_telemetry_json()` 一一对应：
 * ```json
 * {"air_ok":1,"air_t":29.9,"air_rh":59.1,"water_ok":1,"water_t":30.1,
 *  "relay":0,"ain":0,"rssi":-84,"up_s":62}
 * ```
 *
 * **读数无效时设备发 `null` 而不是哨兵值**（固件踩过坑：DS18B20 的
 * 32767 哨兵曾被格式化成 "3276.7" 发给消费端）。所以这里用可空类型。
 */
data class Telemetry(
    val airOk: Boolean,
    val airTempC: Double?,
    val airRh: Double?,
    val waterOk: Boolean,
    val waterTempC: Double?,
    val relayMask: Int,
    val inputMask: Int,
    val rssi: Int,
    val uptimeSec: Long,
) {
    companion object {
        /** 解析一条**完整**的遥测 JSON。失败返回 null（不抛异常，调用方只需跳过）。 */
        fun parse(json: String): Telemetry? = try {
            val o = JSONObject(json)
            Telemetry(
                airOk = o.optInt("air_ok", 0) == 1,
                airTempC = if (o.isNull("air_t")) null else o.optDouble("air_t").takeIf { !it.isNaN() },
                airRh = if (o.isNull("air_rh")) null else o.optDouble("air_rh").takeIf { !it.isNaN() },
                waterOk = o.optInt("water_ok", 0) == 1,
                waterTempC = if (o.isNull("water_t")) null else o.optDouble("water_t").takeIf { !it.isNaN() },
                relayMask = o.optInt("relay", 0),
                inputMask = o.optInt("ain", 0),
                rssi = o.optInt("rssi", 0),
                uptimeSec = o.optLong("up_s", 0),
            )
        } catch (e: Exception) {
            null
        }
    }
}

/**
 * 通知分片重组器。
 *
 * 为什么需要它：单次 BLE 通知的载荷上限是 `MTU - 3`（ATT 头 3 字节）。
 * 设备在**中心设备未协商 MTU** 时会自动分片，每片带 `序号/总数|` 前缀：
 *
 * ```
 * 1/7|{"air_ok":1,"ai
 * 2/7|r_t":29.4,"air_rh
 * ...
 * 7/7|up_s":14}
 * ```
 *
 * 协商到大 MTU 时只有一片，**且设备不发前缀**，就是完整 JSON。
 *
 * 我们两条路都走：连接后请求 MTU 512（正常路径一片到位），
 * 同时保留重组能力（兜底）——否则遇到不协商的客户端就会显示半截 JSON，
 * 而这正是"看着连上了、内容却不对"的经典症状。
 *
 * ⚠️ 这个类是**纯逻辑**，可在 JVM 上单元测试（见 TelemetryAssemblerTest）。
 */
class TelemetryAssembler {

    private var total = 0
    private val parts = HashMap<Int, String>()
    private var lastComplete: String? = null

    /** 供 UI 显示：当前是否处于分片模式。 */
    val isFragmented: Boolean get() = total > 1

    /** 上一片数（0 表示还没收到过分片）。 */
    val fragmentCount: Int get() = total

    /**
     * 喂入一片通知载荷。
     *
     * @return 已拼成完整 JSON 时返回它；还需要更多片、或该片无效时返回 null。
     */
    fun feed(payload: String): String? {
        val m = HEADER.find(payload)
        if (m == null) {
            // 无前缀 —— 单片完整 JSON
            total = 0
            parts.clear()
            lastComplete = payload
            return payload
        }

        val index = m.groupValues[1].toIntOrNull() ?: return null
        val count = m.groupValues[2].toIntOrNull() ?: return null
        if (index < 1 || count < 1 || index > count) return null

        // 新的一批分片开始（序号回到 1），清掉上一批
        if (index == 1) {
            parts.clear()
            total = count
        } else if (count != total) {
            // 与本批总数不符 —— 丢弃，等下一批
            return null
        }

        parts[index] = payload.substring(m.value.length)

        if (parts.size != total) return null

        val sb = StringBuilder()
        for (i in 1..total) {
            val p = parts[i] ?: return null   // 还缺片
            sb.append(p)
        }
        parts.clear()
        lastComplete = sb.toString()
        return lastComplete
    }

    /** 最近一次完整拼好的 JSON（没有则 null）。 */
    fun last(): String? = lastComplete

    private companion object {
        /** 匹配 `n/t|` 前缀，如 "1/7|" */
        val HEADER = Regex("""^(\d{1,2})/(\d{1,2})\|""")
    }
}
