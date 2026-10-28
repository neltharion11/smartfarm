package com.smartfarm.app.ble

import org.json.JSONObject

/**
 * 配网过程消息。
 *
 * 固件会在**配网特征**上通过 Notify 推两类 JSON：
 *
 * 1. 指令回执（写入后立刻回）
 *    `{"ack":"wifi:connect","ok":1}`
 *    `{"ack":"wifi:pass","ok":1,"len":13}`        <- 只含长度，**绝不含密码**
 *    `{"ack":"denied","err":"not allowed on remote channel",...}`
 *
 * 2. 异步连接状态（由 wifi_mgr 驱动，连上/失败时推）
 *    `{"wifi":"connecting","ssid":"809"}`
 *    `{"wifi":"ok","ip":"192.168.2.40","rssi":-45}`
 *    `{"wifi":"fail","reason":"4-way handshake timeout (WRONG PASSWORD?)"}`
 *    `{"wifi":"status","src":"nvs","ssid":"809","connected":1,...}`
 *
 * 这类消息**不抛异常、只静默显示错值**的风险很高（比如把失败看成成功），
 * 所以解析逻辑单独抽出来做单元测试。
 */
sealed interface ProvEvent {

    /** 指令回执 */
    data class Ack(val command: String, val ok: Boolean, val detail: String) : ProvEvent

    /** 连接状态变化 */
    data class State(
        val state: String,
        val detail: String,
        val connected: Boolean? = null,
    ) : ProvEvent

    /** 不是我们认识的配网消息（原样保留，便于排查协议不匹配） */
    data class Unknown(val raw: String) : ProvEvent

    /** 给界面显示的一行文字 */
    fun display(): String = when (this) {
        is Ack -> "${if (ok) "✓" else "✗"} $command${if (detail.isEmpty()) "" else "  $detail"}"
        is State -> when (state) {
            "connecting" -> "正在连接… $detail"
            "ok" -> "✓ 配网成功  $detail"
            "fail" -> "✗ 配网失败  $detail"
            "cleared" -> "凭据已清除"
            "status" -> "状态  $detail"
            else -> "$state  $detail"
        }
        is Unknown -> "? $raw"
    }
}

object WifiProvision {

    fun parse(raw: String): ProvEvent {
        val o = try {
            JSONObject(raw)
        } catch (e: Exception) {
            return ProvEvent.Unknown(raw)
        }

        if (o.has("ack")) {
            val cmd = o.optString("ack")
            // ⚠️ 不能简单地把缺失的 ok 当成成功：拒绝回执形如
            //    {"ack":"denied","err":"...","hint":"..."}   —— 它**没有 ok 字段**，
            // 默认成成功就会把"被拒绝"显示成 ✓。
            // 正确规则：有 ok 就用 ok；没有 ok 则"有 err 就是失败"。
            val ok = if (o.has("ok")) o.optInt("ok") == 1 else !o.has("err")
            val parts = buildList {
                if (o.has("err")) add(o.optString("err"))
                if (o.has("hint")) add(o.optString("hint"))
                // len 只在涉密字段上出现；它**不含密码内容**，可安全显示
                if (o.has("len")) add("长度 ${o.optInt("len")}")
                if (o.has("id")) add("id ${o.optInt("id")}")
            }
            return ProvEvent.Ack(cmd, ok, parts.joinToString(" · "))
        }

        if (o.has("wifi")) {
            val st = o.optString("wifi")
            val detail = when (st) {
                "connecting" -> "SSID ${o.optString("ssid")}"
                "ok" -> "IP ${o.optString("ip")}  信号 ${o.optInt("rssi")} dBm"
                "fail" -> "原因：${o.optString("reason")}"
                "cleared" -> "已清除已保存的 WiFi 凭据"
                // 设备侧的回退：新凭据连续失败后自动恢复上一次可用设置。
                // 必须显式提示，否则用户会以为配网成功了。
                "reverted" -> "⚠ 新凭据连不上，设备已恢复上一次可用设置（${o.optString("reason")}）"
                "status" -> buildString {
                    append("SSID ${o.optString("ssid")}")
                    append("  来源 ${o.optString("src")}")
                    append("  已连接 ${if (o.optInt("connected") == 1) "是" else "否"}")
                    append("  IP ${o.optString("ip")}")
                    append("  信号 ${o.optInt("rssi")} dBm")
                    val fail = o.optString("fail")
                    if (fail.isNotEmpty()) append("  最近失败：$fail")
                }
                else -> ""
            }
            val connected = when (st) {
                "ok" -> true
                "connecting", "fail", "cleared", "reverted" -> false
                "status" -> if (o.has("connected")) o.optInt("connected") == 1 else null
                else -> null
            }
            return ProvEvent.State(st, detail, connected)
        }

        return ProvEvent.Unknown(raw)
    }
}
