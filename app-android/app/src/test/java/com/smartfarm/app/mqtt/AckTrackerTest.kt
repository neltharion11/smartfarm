package com.smartfarm.app.mqtt

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * 指令回执关联 + 超时（C4）。
 *
 * 单测重点全部落在 PLAN C4 用实测换来的三条结论上：
 *   1. 设备没收到指令时**什么都不会回**（MQTT 不缓存），所以"没有回执"必须
 *      判为失败，**绝不能显示成功**；
 *   2. 超时之后再到的回执**不得翻案**成成功；
 *   3. 缺失 ok 字段的回执（拒绝类）**不能当成成功**。
 *
 * 时间由测试显式注入，因此这三条路径都是确定性的（不需要真机与 broker）。
 */
class AckTrackerTest {

    private val ok = "{\"ack\":\"relay\",\"relay\":1,\"bits\":\"100\"}"

    // ---------------- 正常路径 ----------------

    @Test
    fun matchedAckResolvesTheRequest() {
        val t = AckTracker(timeoutMs = 4000)
        assertEquals(1, t.track("relay:100", nowMs = 0))
        val r = t.onAck(ok, nowMs = 120)
        assertTrue(r is CmdOutcome.Acked)
        r as CmdOutcome.Acked
        assertEquals("relay:100", r.command)
        assertTrue("没有 ok 也没有 err 的回执应判为成功", r.ok)
        assertEquals(0, t.pendingCount())
    }

    @Test
    fun multipleInFlightResolveInFifoOrder() {
        // 固件按接收顺序回执，协议里没有请求 id —— FIFO 是唯一可用的关联方式
        val t = AckTracker(timeoutMs = 4000)
        t.track("relay:100", 0)
        t.track("get", 10)
        val a = t.onAck("{\"ack\":\"relay\",\"relay\":1,\"bits\":\"100\"}", 100)
        val b = t.onAck("{\"ack\":\"get\"}", 200)
        assertEquals("relay:100", (a as CmdOutcome.Acked).command)
        assertEquals("get", (b as CmdOutcome.Acked).command)
        assertEquals(0, t.pendingCount())
    }

    // ---------------- 失败关闭：没有回执 = 失败 ----------------

    @Test
    fun noAckWithinDeadlineBecomesTimeout() {
        val t = AckTracker(timeoutMs = 4000)
        t.track("relay:100", nowMs = 0)
        val out = t.pollTimeouts(nowMs = 4001)
        assertEquals(1, out.size)
        assertEquals("relay:100", out[0].command)
        assertEquals(4000L, out[0].waitedMs)
        assertEquals(0, t.pendingCount())
    }

    @Test
    fun deadlineBoundaryIsNotYetTimeout() {
        // now == deadline 还不算超时（边界只能有一侧判超时，否则会出现"刚发就超时"）
        val t = AckTracker(timeoutMs = 4000)
        t.track("get", nowMs = 0)
        assertTrue(t.pollTimeouts(nowMs = 4000).isEmpty())
        assertEquals(1, t.pendingCount())
        assertEquals(1, t.pollTimeouts(nowMs = 4001).size)
    }

    @Test
    fun pollTimeoutsIsIdempotent() {
        val t = AckTracker(timeoutMs = 100)
        t.track("get", 0)
        assertEquals(1, t.pollTimeouts(50).size + t.pollTimeouts(200).size)
        assertTrue("已取走的不应重复返回", t.pollTimeouts(999).isEmpty())
    }

    @Test
    fun lateAckDoesNotTurnIntoSuccess() {
        // 关键：用户在超时那一刻已经看到"设备未响应"，事后再翻成成功会自相矛盾
        val t = AckTracker(timeoutMs = 4000)
        t.track("relay:100", nowMs = 0)
        val r = t.onAck(ok, nowMs = 4500)
        assertTrue("超时后到达的回执必须是 Late，不能是 Acked", r is CmdOutcome.Late)
        assertEquals("relay:100", (r as CmdOutcome.Late).command)
        assertEquals(500L, r.latenessMs)
    }

    @Test
    fun ackAfterTimeoutWasPolledIsUnmatched() {
        val t = AckTracker(timeoutMs = 4000)
        t.track("relay:100", nowMs = 0)
        assertEquals(1, t.pollTimeouts(5000).size)     // 已判定超时并从在途队列移除
        val r = t.onAck(ok, nowMs = 5100)
        assertEquals(CmdOutcome.Unmatched, r)          // 不得复活
    }

    @Test
    fun ackWithoutPendingIsUnmatched() {
        val t = AckTracker()
        assertEquals(CmdOutcome.Unmatched, t.onAck(ok, 0))
    }

    // ---------------- 内容判定：缺失 ok 不等于成功 ----------------

    @Test
    fun denialWithoutOkFieldIsFailure() {
        // 固件拒绝远程配网时的真实回执：**没有 ok 字段**
        val denied = "{\"ack\":\"denied\",\"err\":\"not allowed on remote channel\"," +
                "\"hint\":\"WiFi settings can only be changed over Bluetooth\"}"
        val t = AckTracker()
        t.track("wifi:pass:x", 0)
        val r = t.onAck(denied, 100) as CmdOutcome.Acked
        assertEquals("缺少 ok 但有 err 必须判失败", false, r.ok)
        assertTrue(r.detail.contains("not allowed"))
    }

    @Test
    fun explicitOkZeroIsFailure() {
        // 固件对超长载荷的拒绝回执：{"ack":"mqtt","ok":0,"err":"command too long …"}
        val tooLong = "{\"ack\":\"mqtt\",\"ok\":0,\"err\":\"command too long or malformed\"," +
                "\"len\":300,\"max\":255}"
        val t = AckTracker()
        t.track("rule:add …", 0)
        val r = t.onAck(tooLong, 100) as CmdOutcome.Acked
        assertEquals(false, r.ok)
        assertTrue(r.detail.contains("too long"))
    }

    @Test
    fun nonAckPayloadFailsClosed() {
        // 若误把遥测 JSON 发到了 ack 主题，必须按"失败"处理（失败关闭），
        // 而不是因为"收到了东西"就当作指令成功
        val telemetry = "{\"air_ok\":1,\"air_t\":27.6,\"relay\":0,\"up_s\":42}"
        val t = AckTracker()
        t.track("get", 0)
        val r = t.onAck(telemetry, 50) as CmdOutcome.Acked
        assertEquals(false, r.ok)
        assertTrue(r.detail.contains("不是回执"))
    }

    // ---------------- 生命周期 ----------------

    @Test
    fun clearDropsEverythingInFlight() {
        val t = AckTracker()
        t.track("a", 0)
        t.track("b", 0)
        assertEquals(2, t.pendingCount())
        t.clear()
        assertEquals(0, t.pendingCount())
        assertEquals(CmdOutcome.Unmatched, t.onAck(ok, 10))
    }
}
