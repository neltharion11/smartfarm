package com.smartfarm.app.mqtt

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * WebSocket 地址推导（C4）。
 *
 * 为什么要单测：APP 连不上 broker 时，最难区分的是"网络问题"还是"地址推导错了"。
 * 把推导做成纯函数并钉住，就能把这一类排除掉。
 */
class MqttConfigTest {

    @Test
    fun mqttsWithDefaultPortBecomesWss8884() {
        // HiveMQ Cloud 的 TCP-TLS 地址可转换为 WebSocket-TLS 地址
        assertEquals(
            "wss://147e1cf8.s1.eu.hivemq.cloud:8884/mqtt",
            MqttConfig.toWebSocketUri("mqtts://147e1cf8.s1.eu.hivemq.cloud:8883"),
        )
    }

    @Test
    fun mqttsWithoutPortAlsoBecomesWss8884() {
        assertEquals("wss://b.example:8884/mqtt", MqttConfig.toWebSocketUri("mqtts://b.example"))
    }

    @Test
    fun mqttBecomesWs8083() {
        assertEquals("ws://b.example:8083/mqtt", MqttConfig.toWebSocketUri("mqtt://b.example:1883"))
        assertEquals("ws://b.example:8083/mqtt", MqttConfig.toWebSocketUri("mqtt://b.example"))
    }

    @Test
    fun nonDefaultPortIsKept() {
        // 自建 broker 常把 WebSocket 挂在自定义端口上，不要替换掉
        assertEquals("wss://b.example:9999/mqtt", MqttConfig.toWebSocketUri("mqtts://b.example:9999"))
        assertEquals("ws://b.example:9001/mqtt", MqttConfig.toWebSocketUri("ws://b.example:9001"))
    }

    @Test
    fun alreadyWebSocketUriIsPreserved() {
        assertEquals("wss://b.example:8884/mqtt", MqttConfig.toWebSocketUri("wss://b.example:8884/mqtt"))
        assertEquals("ws://b.example:8083/mqtt", MqttConfig.toWebSocketUri("ws://b.example:8083/mqtt"))
    }

    @Test
    fun missingPathIsFilled() {
        // HiveMQ 的 WebSocket 必须带 /mqtt 路径，漏了会握手失败
        assertEquals("wss://b.example/mqtt", MqttConfig.toWebSocketUri("wss://b.example"))
        assertEquals("wss://b.example:8884/mqtt", MqttConfig.toWebSocketUri("wss://b.example:8884"))
    }

    @Test
    fun customPathIsPreserved() {
        assertEquals("wss://b.example:8884/custom", MqttConfig.toWebSocketUri("wss://b.example:8884/custom"))
    }

    @Test
    fun schemeIsCaseInsensitiveButHostIsNot() {
        assertEquals("wss://B.Example:8884/mqtt", MqttConfig.toWebSocketUri("MQTTS://B.Example:8883"))
    }

    @Test
    fun unusableInputFailsExplicitly() {
        // 明确的失败好过猜一个地址然后让用户对着"连不上"排查
        assertNull(MqttConfig.toWebSocketUri(""))
        assertNull(MqttConfig.toWebSocketUri("   "))
        assertNull(MqttConfig.toWebSocketUri("http://b.example"))
        assertNull(MqttConfig.toWebSocketUri("b.example:8883"))   // 缺 scheme
        assertNull(MqttConfig.toWebSocketUri("mqtts://"))
        assertNull(MqttConfig.toWebSocketUri("mqtts://:8883"))    // 缺 host
    }
}
