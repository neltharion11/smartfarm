package com.smartfarm.app.mqtt

import android.os.SystemClock
import com.smartfarm.app.BuildConfig
import org.eclipse.paho.client.mqttv3.IMqttDeliveryToken
import org.eclipse.paho.client.mqttv3.MqttAsyncClient
import org.eclipse.paho.client.mqttv3.MqttCallbackExtended
import org.eclipse.paho.client.mqttv3.MqttConnectOptions
import org.eclipse.paho.client.mqttv3.MqttException
import org.eclipse.paho.client.mqttv3.MqttMessage
import org.eclipse.paho.client.mqttv3.persist.MemoryPersistence
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledExecutorService
import java.util.concurrent.TimeUnit

/**
 * MQTT over WebSocket 通道（PLAN C4）。
 *
 * 三条用实测换来的硬要求，都在这里落实：
 *
 * 1. **保持一条长连接，不要每条指令重连**。
 *    实测：几分钟内反复建立/断开 MQTT 会话（十几次）后出现**指令间歇性丢失**
 *    （有时回执延迟约 5 秒、有时完全不回，而设备其实活着、遥测一直在流）。
 *    免费版 broker 对会话频繁建立有限流，且表现为"消息丢了"而不是"连接被拒"——
 *    极难排查。所以这里一个 client 反复用，只靠 Paho 的自动重连兜底。
 *
 * 2. **发布指令必须 QoS 1**。MQTT 的投递等级取"发布 QoS 与订阅 QoS 的较小值"，
 *    用 QoS 0 发布时即使订阅方是 QoS 1 也会降级成"尽力而为"。
 *
 * 3. **必须等回执 + 超时**，超时**绝不能显示成功**。
 *    MQTT 不会给"设备尚未订阅的主题"缓存消息；设备离线时发出的指令会静默丢失，
 *    而发布方只会收到 broker 的 PUBACK——那只代表 broker 收到了。
 *    关联与超时判定在 [AckTracker] 里，已单测覆盖。
 *
 * ⚠️ APK 含 MQTT 凭据（构建时由 APP 的 mqtt.properties 注入），属于**含密产物**，不要分发。
 */
class MqttChannel(private val listener: Listener) {

    interface Listener {
        /** 连接状态变化。connected=false 时 UI 应把"能否远程控制"标为不可用。 */
        fun onState(text: String, connected: Boolean)

        /** 设备遥测 JSON（topic: .../telemetry） */
        fun onTelemetry(json: String)

        /** 设备在线状态 JSON（topic: .../status） */
        fun onDeviceStatus(json: String)

        /** 一条指令的最终归属（回执 / 超时 / 迟到 / 多余） */
        fun onOutcome(outcome: CmdOutcome)

        /** 给界面滚动区的一行文字 */
        fun onLog(line: String)
    }

    private var client: MqttAsyncClient? = null
    private val tracker = AckTracker()

    /** 超时轮询：定时器比"每条指令各起一个 Timer"更省资源，也更好推理。 */
    private val scheduler: ScheduledExecutorService = Executors.newSingleThreadScheduledExecutor { r ->
        Thread(r, "mqtt-timeout").apply { isDaemon = true }
    }

    @Volatile
    private var connected = false

    val isConnected: Boolean get() = connected

    /** 在途指令条数（UI 可显示"等待回执 N"） */
    val pendingCount: Int get() = tracker.pendingCount()

    /**
     * 建立连接（非阻塞）。凭据或地址缺失时**立即明确失败**，不让用户干等。
     */
    fun connect() {
        if (client != null) { listener.onState("已连接（复用现有连接）", connected); return }

        if (!MqttConfig.configured) {
            listener.onState("未配置：缺少 MQTT 地址、账号或主题（见文档 C4）", false)
            listener.onLog("✗ 请配置 app-android/mqtt.properties 中的 MQTT 地址、账号和主题")
            return
        }
        val uri = MqttConfig.effectiveUri()
        if (uri == null) {
            val configuredUri = BuildConfig.MQTT_WS_URI.ifEmpty { BuildConfig.MQTT_URI }
            listener.onState("地址无法推导：$configuredUri", false)
            listener.onLog("✗ 无法把 \"$configuredUri\" 推导成 WebSocket 地址")
            return
        }

        // clientId 必须唯一：同名连接会互相踢掉。
        // 用固定前缀 + 时间戳后缀，避免与本机其它实例（如电脑侧探针）冲突。
        val clientId = "smartfarm-app-${System.currentTimeMillis() % 1_000_000}"

        try {
            val c = MqttAsyncClient(uri, clientId, MemoryPersistence())
            val opts = MqttConnectOptions().apply {
                isAutomaticReconnect = true      // 断线自动重连：不靠上层反复 new
                isCleanSession = true            // 每次连上都重新订阅（见 connectComplete）
                connectionTimeout = 10
                keepAliveInterval = 30
                userName = BuildConfig.MQTT_USER
                password = BuildConfig.MQTT_PASS.toCharArray()
                // 服务器证书用系统信任库校验（HiveMQ 用公共 CA）。
                // 绝不 setSocketFactory(trust-all)——那等于关掉 TLS 的意义。
            }
            c.setCallback(object : MqttCallbackExtended {
                override fun connectComplete(reconnect: Boolean, serverURI: String?) {
                    connected = true
                    listener.onState(if (reconnect) "已重连" else "已连接", true)
                    // cleanSession=true 时服务端不保留订阅，必须**每次连上都重新订阅**
                    subscribeAll(c)
                }

                override fun connectionLost(cause: Throwable?) {
                    connected = false
                    // 不 clear tracker：在途指令要靠超时给出"未响应"，
                    // 而不是被静默丢弃（那正是"看起来成功"的来源）。
                    listener.onState("连接断开：${cause?.message ?: "未知原因"}", false)
                    listener.onLog("✗ 连接断开（自动重连中）")
                }

                override fun messageArrived(topic: String?, message: MqttMessage?) {
                    val payload = message?.payload?.toString(Charsets.UTF_8) ?: return
                    val t = topic ?: return
                    when {
                        t == MqttConfig.ackTopic -> {
                            // 收到回执才结算；判定顺序（先超时后内容）在 AckTracker 内
                            listener.onOutcome(tracker.onAck(payload, now()))
                        }
                        t == MqttConfig.telemetryTopic -> listener.onTelemetry(payload)
                        t == MqttConfig.statusTopic -> listener.onDeviceStatus(payload)
                        else -> listener.onLog("? 未知主题 $t")
                    }
                }

                override fun deliveryComplete(token: IMqttDeliveryToken?) { /* QoS1 上行确认，无需展示 */ }
            })
            client = c

            listener.onState("连接中…", false)
            listener.onLog("broker  $uri")
            c.connect(opts, null, object : org.eclipse.paho.client.mqttv3.IMqttActionListener {
                override fun onSuccess(asyncActionToken: org.eclipse.paho.client.mqttv3.IMqttToken?) {
                    listener.onLog("✓ CONNECT 已发送，等待 CONNACK")
                }

                override fun onFailure(
                    asyncActionToken: org.eclipse.paho.client.mqttv3.IMqttToken?,
                    exception: Throwable?,
                ) {
                    connected = false
                    listener.onState("连接失败：${exception?.message ?: "未知"}", false)
                    listener.onLog("✗ 连接失败：${exception?.message ?: "未知"}")
                }
            })

            // 超时轮询：每 200 ms 结算一次到期的在途指令
            scheduler.scheduleWithFixedDelay({
                try {
                    tracker.pollTimeouts(now()).forEach { listener.onOutcome(it) }
                } catch (_: Throwable) {
                    // 轮询线程绝不能因异常而静默死掉
                }
            }, 200, 200, TimeUnit.MILLISECONDS)
        } catch (e: MqttException) {
            listener.onState("初始化失败：${e.message}", false)
            listener.onLog("✗ MqttAsyncClient 初始化失败：${e.message}")
        }
    }

    private fun subscribeAll(c: MqttAsyncClient) {
        // ack 用 QoS1（与指令同级）；遥测/状态用 QoS1 也可以，但它们是持续流，
        // QoS0 足够且更省——丢一条遥测下一帧就补上了。
        try {
            c.subscribe(MqttConfig.ackTopic, 1)
            c.subscribe(MqttConfig.telemetryTopic, 1)
            c.subscribe(MqttConfig.statusTopic, 1)
            listener.onLog("✓ 已订阅 ack / telemetry / status")
        } catch (e: MqttException) {
            listener.onLog("✗ 订阅失败：${e.message}")
        }
    }

    /**
     * 发送一条指令（QoS 1）。返回 false 表示**根本没发出去**——调用方应据此
     * 立刻提示，而不是记一笔在途然后等超时（那样提示会晚 4 秒且原因不准）。
     */
    fun send(command: String): Boolean {
        val c = client
        if (c == null || !connected) {
            listener.onLog("✗ 未连接，指令未发送：$command")
            return false
        }
        return try {
            // 参数：topic, payload, qos=1, retained=false
            c.publish(MqttConfig.cmdTopic, command.toByteArray(Charsets.UTF_8), 1, false)
            // 先发后记：ack 要经过 broker→设备→broker→APP，往返远长于这两行的间隔，
            // 因此不存在"回执先到、记录后到"的竞态。
            val inFlight = tracker.track(command, now())
            listener.onLog("→ $command   (QoS1，在途 $inFlight)")
            true
        } catch (e: MqttException) {
            listener.onLog("✗ 发布失败：${e.message}")
            false
        }
    }

    fun close() {
        connected = false
        try { scheduler.shutdownNow() } catch (_: Throwable) {}
        try { client?.disconnect() } catch (_: Throwable) {}
        try { client?.close() } catch (_: Throwable) {}
        client = null
        tracker.clear()
    }

    /** 单调时钟：不受系统时间被改动的影响（超时判定必须用单调时间）。 */
    private fun now(): Long = SystemClock.elapsedRealtime()
}
