package com.smartfarm.app.mqtt

import com.smartfarm.app.BuildConfig

/**
 * MQTT over WebSocket 的地址推导（PLAN C4）。
 *
 * 为什么用 WebSocket 而不是裸 TCP：
 *   HiveMQ Cloud 只提供两个 TLS 入口，两个都实测可达：
 *     mqtts://<host>:8883      TCP + TLS
 *     wss://<host>:8884/mqtt   WebSocket + TLS
 *   裸 TCP 在移动网络/部分运营商下更容易被中间设备干扰，WebSocket 走 443 风格的
 *   升级握手，兼容性更好（PLAN C4 的选择）。
 *
 * 地址、主题与账号来自 APP 自己的 `app-android/mqtt.properties`，由 Gradle 注入
 * BuildConfig。该文件缺失时 APP 仍可编译，MQTT 卡片显示"未配置"。
 */
object MqttConfig {

    /** 主题前缀。配置时须与设备实际使用的主题保持一致。 */
    val topicRoot: String get() = BuildConfig.TOPIC_ROOT

    val cmdTopic: String get() = "$topicRoot/cmd"
    val ackTopic: String get() = "$topicRoot/ack"
    val telemetryTopic: String get() = "$topicRoot/telemetry"
    val statusTopic: String get() = "$topicRoot/status"

    /** 凭据是否齐全（不齐时 UI 直接说明原因，而不是让用户干等连接失败）。 */
    val configured: Boolean
        get() = (BuildConfig.MQTT_WS_URI.isNotEmpty() || BuildConfig.MQTT_URI.isNotEmpty()) &&
            BuildConfig.MQTT_USER.isNotEmpty() && BuildConfig.TOPIC_ROOT.isNotEmpty()

    /** 实际使用的地址：优先用显式 WebSocket 地址，否则由 TCP 地址推导。 */
    fun effectiveUri(): String? =
        if (BuildConfig.MQTT_WS_URI.isNotEmpty()) toWebSocketUri(BuildConfig.MQTT_WS_URI)
        else toWebSocketUri(BuildConfig.MQTT_URI)

    /**
     * 把配置中的 MQTT 地址转换成 WebSocket 形式。
     *
     * 规则（纯函数，单元测试覆盖）：
     *   `mqtts://h` / `mqtts://h:8883`  ->  `wss://h:8884/mqtt`
     *   `mqtt://h`  / `mqtt://h:1883`   ->  `ws://h:8083/mqtt`   （本地 broker 的常见约定）
     *   `ws://…` / `wss://…`            ->  原样返回，缺 path 时补 `/mqtt`
     *   其它 scheme、空串、无 host      ->  null（**明确失败**，不猜）
     *
     * 端口不是默认值时保留原端口（自建 broker 常把 WS 挂在自定义端口上）。
     *
     * @return 可用的 WebSocket 地址；无法推导时返回 null。
     */
    fun toWebSocketUri(raw: String): String? {
        val s = raw.trim()
        if (s.isEmpty()) return null

        val schemeEnd = s.indexOf("://")
        if (schemeEnd <= 0) return null
        val scheme = s.substring(0, schemeEnd).lowercase()
        val rest = s.substring(schemeEnd + 3)
        if (rest.isEmpty()) return null

        val slash = rest.indexOf('/')
        val authority = if (slash >= 0) rest.substring(0, slash) else rest
        val path = if (slash >= 0) rest.substring(slash) else ""
        if (authority.isEmpty()) return null

        val colon = authority.lastIndexOf(':')
        val host = if (colon >= 0) authority.substring(0, colon) else authority
        val portStr = if (colon >= 0) authority.substring(colon + 1) else ""
        if (host.isEmpty()) return null
        val port = portStr.toIntOrNull()

        val hostPort = { p: Int? -> if (p == null) host else "$host:$p" }

        return when (scheme) {
            "wss" -> "wss://${hostPort(port)}${path.ifEmpty { "/mqtt" }}"
            "ws" -> "ws://${hostPort(port)}${path.ifEmpty { "/mqtt" }}"
            "mqtts", "ssl" -> {
                val p = port ?: 8883
                // HiveMQ Cloud：TCP-TLS 8883 对应的 WebSocket-TLS 是 8884
                "wss://${hostPort(if (p == 8883) 8884 else p)}${path.ifEmpty { "/mqtt" }}"
            }
            "mqtt", "tcp" -> {
                val p = port ?: 1883
                "ws://${hostPort(if (p == 1883) 8083 else p)}${path.ifEmpty { "/mqtt" }}"
            }
            else -> null
        }
    }
}
