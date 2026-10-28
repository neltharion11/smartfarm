package com.smartfarm.app.ble

/** 写入回执只表示设备接收指令；只有连接状态才能确认配网成功。 */
data class ProvisionProgress(
    val phase: Phase = Phase.IDLE,
    val message: String = "尚未确认 WiFi 状态，已配网的设备可直接进入设备控制",
) {
    enum class Phase { IDLE, WAITING, SUCCESS, FAILED, REVERTED }

    val waiting: Boolean get() = phase == Phase.WAITING
    val succeeded: Boolean get() = phase == Phase.SUCCESS

    fun begin(ssid: String) = ProvisionProgress(Phase.WAITING, "正在为 $ssid 配网，等待设备确认连接…")

    fun onEvent(event: ProvEvent): ProvisionProgress = when (event) {
        is ProvEvent.Ack -> if (!event.ok) {
            ProvisionProgress(Phase.FAILED, event.display())
        } else this
        is ProvEvent.State -> when {
            event.state == "reverted" -> ProvisionProgress(Phase.REVERTED, event.detail)
            event.state == "cleared" -> ProvisionProgress(Phase.IDLE, event.display())
            // 回退后即使旧网络连上，也不能把本次新凭据判为配网成功。
            phase == Phase.REVERTED -> this
            event.state == "ok" || (event.state == "status" && event.connected == true) ->
                ProvisionProgress(Phase.SUCCESS, "✓ WiFi 已连接  ${event.detail}")
            event.state == "fail" -> ProvisionProgress(Phase.FAILED, event.display())
            event.state == "connecting" -> ProvisionProgress(Phase.WAITING, event.display())
            event.state == "status" -> copy(message = event.display())
            else -> this
        }
        is ProvEvent.Unknown -> this
    }

    fun timeout(): ProvisionProgress = if (waiting) {
        ProvisionProgress(Phase.FAILED, "尚未收到连接成功确认，请查询状态，或检查 WiFi 名称和密码后重试")
    } else this
}
