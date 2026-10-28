package com.smartfarm.app.mqtt

import com.smartfarm.app.ble.ProvEvent
import com.smartfarm.app.ble.WifiProvision

/**
 * 一条指令的最终归属。
 *
 * ⚠️ **超时绝不能伪装成成功**——这是 PLAN C4 用实测换来的硬要求：
 * MQTT 不会给"设备尚未订阅的主题"缓存消息。设备离线、正在重连、或刚开机还没
 * 订阅时发出的指令会**静默丢失**，而发布方（QoS 1）只会收到 broker 的 PUBACK——
 * 那只代表 **broker 收到了**，不代表**设备收到了**。
 */
sealed interface CmdOutcome {
    /** 收到回执。ok 由回执内容判定（见 [AckTracker.ackVerdict]）。 */
    data class Acked(
        val command: String,
        val ok: Boolean,
        val detail: String,
        val raw: String,
    ) : CmdOutcome

    /** 超过时限仍未收到回执 → 界面必须显示"设备未响应"。 */
    data class TimedOut(val command: String, val waitedMs: Long) : CmdOutcome

    /** 超时**之后**才到达的回执：不得翻案成成功（用户已经看到"未响应"了）。 */
    data class Late(val command: String, val latenessMs: Long) : CmdOutcome

    /** 没有待处理指令却收到回执：多余的 / 上一轮的迟到件。同样**不算成功**。 */
    data object Unmatched : CmdOutcome
}

/**
 * 指令 ↔ 回执的关联与超时。
 *
 * 为什么是 FIFO：固件对每条指令都回一条 ack，且**按接收顺序回**，协议里没有请求 id
 * （见固件 `cmd.h` / `mqtt_app.c`）。因此"最早的待处理项 ↔ 最早到达的 ack"
 * 是唯一可用的关联方式。这个假设必须写在这里——它是本类的正确性前提。
 *
 * 本类**不依赖 Android、也不读系统时钟**：时间由调用方以 `nowMs` 传入。
 * 这样"超时""超时后才到""多余回执"这三条最容易出错的路径都能在 JVM 上
 * 确定性地验证，而不需要真机和 broker。
 */
class AckTracker(private val timeoutMs: Long = DEFAULT_TIMEOUT_MS) {

    /** 嵌套类（非 inner）不能访问外层的 timeoutMs，所以 deadline 在 track 时算好存进来。 */
    private class Pending(val command: String, val sentAtMs: Long, val deadlineMs: Long)

    private val pending = ArrayDeque<Pending>()

    /** 记录一条刚发出的指令。返回当前在途条数（便于 UI 显示）。 */
    fun track(command: String, nowMs: Long): Int {
        pending.addLast(Pending(command, nowMs, nowMs + timeoutMs))
        return pending.size
    }

    /**
     * 收到一条 ack，返回它的归属判定。
     *
     * ⚠️ 判定顺序很重要：**先判超时，再解析内容**。反过来的话，一条在超时后才到的
     * "成功"回执会把已经显示给用户的"设备未响应"翻成成功。
     */
    fun onAck(raw: String, nowMs: Long): CmdOutcome {
        val p = pending.removeFirstOrNull() ?: return CmdOutcome.Unmatched
        if (nowMs > p.deadlineMs) {
            return CmdOutcome.Late(p.command, nowMs - p.deadlineMs)
        }
        val (ok, detail) = ackVerdict(raw)
        return CmdOutcome.Acked(p.command, ok, detail, raw)
    }

    /**
     * 取出所有已超时的在途指令（调用方定期调用，并把结果呈现为"设备未响应"）。
     * 幂等：已被取走的不会再返回。
     */
    fun pollTimeouts(nowMs: Long): List<CmdOutcome.TimedOut> {
        val out = mutableListOf<CmdOutcome.TimedOut>()
        while (true) {
            val head = pending.firstOrNull() ?: break
            if (nowMs <= head.deadlineMs) break
            pending.removeFirst()
            out += CmdOutcome.TimedOut(head.command, head.deadlineMs - head.sentAtMs)
        }
        return out
    }

    fun pendingCount(): Int = pending.size

    fun clear() = pending.clear()

    companion object {
        /** PLAN 建议 3–5 秒；取中间值 4 秒。 */
        const val DEFAULT_TIMEOUT_MS = 4000L

        /**
         * 回执判定：**绝不能把缺失的 ok 当成成功**。
         *
         * 复用 BLE 侧已经单测过的规则（同一套线格式）：
         *   {"ack":"relay","relay":1,"bits":"100"}   没有 ok，也没有 err -> 成功
         *   {"ack":"denied","err":"not allowed …"}   没有 ok，但有 err   -> 失败
         *   {"ack":"mqtt","ok":0,"err":"command too long …"}           -> 失败
         *
         * 若载荷根本不是回执（例如误把遥测 JSON 发到了 ack 主题），
         * 按**失败关闭**处理（ok=false + 说明），不猜成成功。
         */
        fun ackVerdict(raw: String): Pair<Boolean, String> =
            when (val ev = WifiProvision.parse(raw)) {
                is ProvEvent.Ack -> ev.ok to ev.detail
                else -> false to "不是回执（协议不符）：$raw"
            }
    }
}
