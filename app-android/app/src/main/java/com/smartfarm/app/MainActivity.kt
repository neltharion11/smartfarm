package com.smartfarm.app

import android.Manifest
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import com.smartfarm.app.ble.ProvEvent
import com.smartfarm.app.ble.ProvisionProgress
import com.smartfarm.app.ble.SmartFarmBle
import com.smartfarm.app.ble.Telemetry
import com.smartfarm.app.channel.ChannelRouter
import com.smartfarm.app.mqtt.AckTracker
import com.smartfarm.app.mqtt.CmdOutcome
import com.smartfarm.app.mqtt.MqttChannel

/**
 * C2 + C3：蓝牙实时数据 + 蓝牙配网。
 *
 * C3 的关键差异：**配网特征在固件里要求加密链路（WRITE_ENC）**，
 * 所以写它时手机会弹出配对、要求输入设备显示的 6 位 Passkey。
 * 客户端已经处理了"写入被拒 → 发起配对 → 配对完成自动重试"的流程。
 *
 * 遥测特征仍然不需要配对，所以不配网也能看数据。
 */
class MainActivity : ComponentActivity() {

    private var ble: SmartFarmBle? = null

    private val status = mutableStateOf("未连接")
    private val telemetry = mutableStateOf<Telemetry?>(null)
    private val linkInfo = mutableStateOf("")
    private val error = mutableStateOf<String?>(null)
    private val scanning = mutableStateOf(false)

    private val ssid = mutableStateOf("")
    private val password = mutableStateOf("")
    private val provLog = mutableStateListOf<String>()
    private enum class Page { CONTROL, PROVISION }
    private val page = mutableStateOf(Page.CONTROL)
    private val provisionProgress = mutableStateOf(ProvisionProgress())
    private val uiHandler = Handler(Looper.getMainLooper())
    private val provisionTimeout = Runnable {
        provisionProgress.value = provisionProgress.value.timeout()
    }

    // ---- C4：MQTT 远程通道 ----
    private var mqtt: MqttChannel? = null
    private val mqttState = mutableStateOf("未连接")
    private val mqttConnected = mutableStateOf(false)
    private val mqttLog = mutableStateListOf<String>()
    private val cmdText = mutableStateOf("")
    private val deviceOnline = mutableStateOf<Boolean?>(null)
    private val mqttTelemetry = mutableStateOf<Telemetry?>(null)

    // ---- C5：通道路由 ----
    private val bleConnected = mutableStateOf(false)
    private val bleReady = mutableStateOf(false)

    private val permissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { grants ->
        val denied = grants.filterValues { granted -> !granted }.keys
        if (denied.isEmpty()) beginScan()
        else error.value = "缺少权限：${denied.joinToString()}\n" +
                "蓝牙扫描需要「附近的设备」权限（不是定位）。"
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        ble = SmartFarmBle(this, object : SmartFarmBle.Listener {
            override fun onStatus(text: String) {
                runOnUiThread {
                    status.value = text
                    if (text.startsWith("已订阅") || text.startsWith("已就绪")) scanning.value = false
                    if (text.startsWith("已断开")) scanning.value = false
                    if (text.startsWith("已下发") || text.startsWith("正在配对") ||
                        text.startsWith("配对")) {
                        addLog(text)
                    }
                }
            }

            override fun onTelemetry(t: Telemetry, mtu: Int, fragments: Int) {
                runOnUiThread {
                    telemetry.value = t
                    linkInfo.value = "MTU=$mtu  单次上限=${mtu - 3}B  分片=$fragments"
                    error.value = null
                }
            }

            override fun onProvision(ev: ProvEvent) {
                runOnUiThread {
                    addLog(ev.display())
                    val previous = provisionProgress.value
                    val next = previous.onEvent(ev)
                    provisionProgress.value = next
                    if (!next.waiting) uiHandler.removeCallbacks(provisionTimeout)
                    else if (!previous.waiting) uiHandler.postDelayed(provisionTimeout, 60_000L)
                    if (next.succeeded && !previous.succeeded) {
                        password.value = ""
                        error.value = null
                        page.value = Page.CONTROL
                        connectRemote()
                    }
                }
            }

            override fun onCommandAck(raw: String) {
                // ⚠️ 与 MQTT 通道**共用同一套判定**（含"缺失 ok 字段 ≠ 成功"），
                // 否则同一条回执在两条通道上会得出不同结论——那是经典的分叉缺陷。
                val (ok, detail) = AckTracker.ackVerdict(raw)
                runOnUiThread {
                    addCmdLog("${if (ok) "✓" else "✗"}[BLE] ${detail.ifEmpty { raw }}")
                }
            }

            override fun onLinkState(connected: Boolean, ready: Boolean) {
                runOnUiThread {
                    bleConnected.value = connected
                    bleReady.value = ready
                    // 断连后不能把上一帧读数继续显示成实时。
                    if (!connected) {
                        telemetry.value = null
                        if (provisionProgress.value.waiting) {
                            provisionProgress.value = ProvisionProgress(
                                ProvisionProgress.Phase.FAILED,
                                "蓝牙已断开，尚未确认配网结果。请重新连接后查询状态",
                            )
                            uiHandler.removeCallbacks(provisionTimeout)
                        }
                    }
                }
            }

            override fun onError(text: String) {
                runOnUiThread {
                    error.value = text
                    addLog("✗ $text")
                    scanning.value = false
                    if (provisionProgress.value.waiting) {
                        provisionProgress.value = ProvisionProgress(ProvisionProgress.Phase.FAILED, text)
                        uiHandler.removeCallbacks(provisionTimeout)
                    }
                }
            }
        })

        startMqtt()

        setContent {
            MaterialTheme {
                Surface(
                    modifier = Modifier.fillMaxSize(),
                    color = MaterialTheme.colorScheme.background,
                ) { Screen() }
            }
        }
    }

    /**
     * C4 的要点全在这里：**收到回执才算数，超时明确报"设备未响应"**。
     * 绝不能因为"发布成功"就显示成功——那只证明 broker 收到了。
     */
    private fun startMqtt() {
        mqtt = MqttChannel(object : MqttChannel.Listener {
            override fun onState(text: String, connected: Boolean) {
                runOnUiThread {
                    mqttState.value = text
                    mqttConnected.value = connected
                    if (!connected) mqttTelemetry.value = null
                }
            }

            override fun onTelemetry(json: String) {
                runOnUiThread {
                    Telemetry.parse(json)?.let { mqttTelemetry.value = it }
                }
            }

            override fun onDeviceStatus(json: String) {
                runOnUiThread {
                    deviceOnline.value = json.contains("\"online\":true")
                }
            }

            override fun onOutcome(outcome: CmdOutcome) {
                runOnUiThread { addCmdLog(describe(outcome)) }
            }

            override fun onLog(line: String) {
                runOnUiThread { addCmdLog(line) }
            }
        })
    }

    private fun connectRemote() {
        if (mqtt == null) startMqtt()
        mqtt?.connect()
    }

    private fun disconnectRemote() {
        mqtt?.close()
        mqtt = null
        mqttConnected.value = false
        mqttTelemetry.value = null
        deviceOnline.value = null
        mqttState.value = "已断开"
    }

    /** 把一次指令的最终归属渲染成一行可读文字（超时/迟到都必须显式区别于成功）。 */
    private fun describe(o: CmdOutcome): String = when (o) {
        is CmdOutcome.Acked ->
            if (o.ok) "✓ ${o.command}  回执：${o.detail.ifEmpty { "ok" }}"
            else "✗ ${o.command}  ${o.detail}"
        is CmdOutcome.TimedOut ->
            "⏱ ${o.command}  设备未响应（${o.waitedMs} ms 内无回执）"
        is CmdOutcome.Late ->
            "⚠ ${o.command}  回执迟到 ${o.latenessMs} ms —— 已按未响应处理，不翻案为成功"
        CmdOutcome.Unmatched ->
            "? 收到多余回执（当前没有在途指令）"
    }

    private fun addCmdLog(line: String) {
        mqttLog.add(line)
        while (mqttLog.size > 12) mqttLog.removeAt(0)
    }

    /**
     * C5：按当前链路自动选通道并下发控制指令。
     *
     * 两条通道**语义完全一致**（固件侧走同一个 `cmd_execute`），差别只在传输：
     *  · 蓝牙是本地可靠链路（带响应写）——写成功即代表设备收到了；
     *  · MQTT 要经过 broker，设备可能不在线，所以必须依赖回执 + 超时（见 C4）。
     * 这个差别是**有意保留**的：给蓝牙硬套一个"设备未响应"超时反而会误导，
     * 因为蓝牙链路上不存在"设备离线但链路还在"这种状态。
     */
    private fun sendControl(cmd: String) {
        val d = ChannelRouter.choose(bleReady = bleReady.value, mqttConnected = mqttConnected.value)
        when (d.channel) {
            ChannelRouter.Channel.BLE -> {
                addCmdLog("→[BLE] $cmd")
                ble?.writeCommand(cmd)
            }
            ChannelRouter.Channel.MQTT -> {
                if (mqtt?.send(cmd) != true) addCmdLog("✗ 未发送：$cmd")
            }
            ChannelRouter.Channel.NONE -> {
                error.value = d.reason
                addCmdLog("✗ 无可用通道：$cmd")
            }
        }
    }

    override fun onDestroy() {
        uiHandler.removeCallbacksAndMessages(null)
        ble?.closeGatt()
        mqtt?.close()
        super.onDestroy()
    }

    private fun addLog(line: String) {
        provLog.add(line)
        while (provLog.size > 12) provLog.removeAt(0)   // 只留最近 12 条
    }

    private fun requiredPermissions(): Array<String> =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        } else {
            arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
        }

    private fun requestScan() {
        error.value = null
        val need = requiredPermissions().filter {
            checkSelfPermission(it) != android.content.pm.PackageManager.PERMISSION_GRANTED
        }
        if (need.isEmpty()) beginScan() else permissionLauncher.launch(need.toTypedArray())
    }

    private fun beginScan() {
        telemetry.value = null
        linkInfo.value = ""
        provLog.clear()
        scanning.value = true
        ble?.startScan()
    }

    /** 依次下发配网三步。指令由客户端队列串行发送，顺序有保证。 */
    private fun provision() {
        if (!bleReady.value) { error.value = "请先扫描并连接蓝牙设备"; return }
        if (provisionProgress.value.waiting) return
        val s = ssid.value.trim()
        if (s.isEmpty()) { error.value = "请先填写 WiFi 名称（SSID）"; return }
        error.value = null
        provisionProgress.value = provisionProgress.value.begin(s)
        uiHandler.removeCallbacks(provisionTimeout)
        uiHandler.postDelayed(provisionTimeout, 60_000L)
        addLog("开始配网：$s")
        ble?.writeWifiCommand("wifi:ssid:$s")
        // 密码可能为空（开放网络），空也要发，以便清掉上一次的残留
        ble?.writeWifiCommand("wifi:pass:${password.value}")
        ble?.writeWifiCommand("wifi:connect")
    }

    @Composable
    private fun Screen() {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .safeDrawingPadding()
                .imePadding()
                .padding(20.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            Text("智慧农场 · ${BuildConfig.VERSION_NAME}", style = MaterialTheme.typography.headlineSmall)

            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Button(
                    onClick = { page.value = Page.CONTROL },
                    enabled = page.value != Page.CONTROL,
                    modifier = Modifier.weight(1f),
                ) { Text("设备控制") }
                Button(
                    onClick = { page.value = Page.PROVISION },
                    enabled = page.value != Page.PROVISION,
                    modifier = Modifier.weight(1f),
                ) { Text("WiFi 配网") }
            }

            key(page.value) {
                Column(
                    modifier = Modifier.weight(1f).verticalScroll(rememberScrollState()),
                    verticalArrangement = Arrangement.spacedBy(12.dp),
                ) {
                    ConnectionSection()
                    if (page.value == Page.CONTROL) {
                        ControlSection()
                    } else {
                        ProvisionCard()
                        Text(
                            "首次配网需要在手机配对弹窗中输入 6 位 PIN 码。" +
                                    "设备确认 WiFi 连接成功后，会自动进入设备控制。",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }

                    error.value?.let { e ->
                        Card(modifier = Modifier.fillMaxWidth()) {
                            Text(e, color = Color(0xFFB00020),
                                style = MaterialTheme.typography.bodySmall,
                                modifier = Modifier.padding(12.dp))
                        }
                    }
                }
            }
        }
    }

    @Composable
    private fun ConnectionSection() {
        Button(
            onClick = {
                if (scanning.value) {
                    ble?.closeGatt(); scanning.value = false; status.value = "已停止"
                } else requestScan()
            },
            modifier = Modifier.fillMaxWidth(),
        ) { Text(if (scanning.value) "停止" else "扫描并连接设备") }

        Text("状态：${status.value}", style = MaterialTheme.typography.bodyMedium)
        Text(
            provisionProgress.value.message,
            style = MaterialTheme.typography.bodyMedium,
            color = when (provisionProgress.value.phase) {
                ProvisionProgress.Phase.SUCCESS -> Color(0xFF2E7D32)
                ProvisionProgress.Phase.FAILED, ProvisionProgress.Phase.REVERTED -> Color(0xFFB00020)
                else -> MaterialTheme.colorScheme.onSurfaceVariant
            },
        )
    }

    @Composable
    private fun ControlSection() {
        // 蓝牙断连后自动使用 MQTT 的遥测，开关仍以设备上报为准。
        val live = telemetry.value ?: mqttTelemetry.value
        RelayCard(live)
        telemetry.value?.let { TelemetryCard(it, "实时数据（蓝牙）") }
        if (linkInfo.value.isNotEmpty()) {
            Text(linkInfo.value, style = MaterialTheme.typography.bodySmall,
                fontFamily = FontFamily.Monospace)
        }
        MqttCard()
        mqttTelemetry.value?.let { TelemetryCard(it, "实时数据（MQTT）") }
    }

    @Composable
    private fun ProvisionCard() {
        Card(modifier = Modifier.fillMaxWidth()) {
            Column(
                modifier = Modifier.padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Text("WiFi 配网（通过蓝牙下发）", style = MaterialTheme.typography.titleMedium)

                OutlinedTextField(
                    value = ssid.value,
                    onValueChange = { ssid.value = it },
                    label = { Text("WiFi 名称（SSID）") },
                    enabled = !provisionProgress.value.waiting,
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth(),
                )
                OutlinedTextField(
                    value = password.value,
                    onValueChange = { password.value = it },
                    label = { Text("WiFi 密码") },
                    enabled = !provisionProgress.value.waiting,
                    singleLine = true,
                    visualTransformation = PasswordVisualTransformation(),
                    modifier = Modifier.fillMaxWidth(),
                )

                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(
                        onClick = { provision() },
                        enabled = bleReady.value && !provisionProgress.value.waiting,
                    ) { Text(if (provisionProgress.value.waiting) "配网中…" else "开始配网") }
                    OutlinedButton(
                        onClick = { ble?.writeWifiCommand("wifi:status") },
                        enabled = bleReady.value && !provisionProgress.value.waiting,
                    ) {
                        Text("查询状态")
                    }
                }

                OutlinedButton(onClick = { page.value = Page.CONTROL }) {
                    Text("进入设备控制")
                }

                if (provLog.isNotEmpty()) {
                    Spacer(Modifier.height(4.dp))
                    Text("过程：", style = MaterialTheme.typography.bodySmall)
                    provLog.forEach { line ->
                        Text(
                            line,
                            style = MaterialTheme.typography.bodySmall,
                            fontFamily = FontFamily.Monospace,
                            color = when {
                                line.startsWith("✓") -> Color(0xFF2E7D32)
                                line.startsWith("✗") -> Color(0xFFB00020)
                                else -> MaterialTheme.colorScheme.onSurface
                            },
                        )
                    }
                }
            }
        }
    }

    /**
     * C5：继电器控制。
     *
     * 开关状态取自**当前可用**的那条通道的遥测（蓝牙优先，蓝牙断连后自动落到 MQTT），
     * 所以"走进走出"时数据不断流。
     *
     * 遥测为 null 时开关**禁用**：宁可让用户看到"状态未知"，
     * 也不能拿一个猜出来的状态去发反向指令（那会把水泵从关拨到开）。
     */
    @Composable
    private fun RelayCard(live: Telemetry?) {
        val cur = live?.relayMask ?: 0
        val decision = ChannelRouter.choose(bleReady.value, mqttConnected.value)

        Card(modifier = Modifier.fillMaxWidth()) {
            Column(
                modifier = Modifier.padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Text("继电器控制", style = MaterialTheme.typography.titleMedium)
                Text(
                    "当前通道：${decision.channel} —— ${decision.reason}",
                    style = MaterialTheme.typography.bodySmall,
                    color = if (decision.usable) MaterialTheme.colorScheme.onSurfaceVariant
                    else Color(0xFFB00020),
                )

                for (i in 0..2) {
                    val on = ((cur shr i) and 1) == 1
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.SpaceBetween,
                    ) {
                        Text("继电器 ${i + 1}", style = MaterialTheme.typography.bodyLarge)
                        Switch(
                            checked = on,
                            enabled = live != null && decision.usable,
                            onCheckedChange = { want ->
                                // 三路合成**一条**指令 relay:XYZ（1 位一路）。
                                // 固件里 relay:100 才是"全量设定"，逐路单发会互相覆盖。
                                val bits = (0..2).joinToString("") { b ->
                                    val v = if (b == i) (if (want) 1 else 0) else (cur shr b) and 1
                                    v.toString()
                                }
                                sendControl("relay:$bits")
                            },
                        )
                    }
                }

                Text(
                    if (live == null) "尚无遥测：开关状态未知，故禁用（先连接蓝牙或 MQTT）"
                    else "设备上报 继电器 ${bits(live.relayMask)}   输入 ${bits(live.inputMask)}",
                    style = MaterialTheme.typography.bodySmall,
                )
            }
        }
    }

    /**
     * C4：MQTT over WebSocket 远程通道。
     *
     * 界面刻意把三种结果分色显示：成功（绿）/ 失败（红）/ 超时（红，且写明
     * "设备未响应"）/ 迟到（橙，并注明**不翻案为成功**）。
     * 这正是 PLAN C4 的要求：设备断电时点控制按钮，APP 必须显示"设备未响应"，
     * 而不是假装成功。
     */
    @Composable
    private fun MqttCard() {
        Card(modifier = Modifier.fillMaxWidth()) {
            Column(
                modifier = Modifier.padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                Text("远程控制（MQTT over WebSocket）", style = MaterialTheme.typography.titleMedium)
                Text("状态：${mqttState.value}", style = MaterialTheme.typography.bodyMedium)
                Text(
                    when (deviceOnline.value) {
                        true -> "设备在线"
                        false -> "设备离线（给它发指令只会得到「设备未响应」）"
                        null -> "设备状态未知（尚未收到 status）"
                    },
                    style = MaterialTheme.typography.bodySmall,
                    color = when (deviceOnline.value) {
                        true -> Color(0xFF2E7D32)
                        false -> Color(0xFFB00020)
                        null -> MaterialTheme.colorScheme.onSurfaceVariant
                    },
                )

                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(onClick = { connectRemote() }, enabled = !mqttConnected.value) {
                        Text("连接")
                    }
                    OutlinedButton(
                        onClick = { disconnectRemote() },
                        enabled = mqttConnected.value,
                    ) { Text("断开") }
                }

                OutlinedTextField(
                    value = cmdText.value,
                    onValueChange = { cmdText.value = it },
                    label = { Text("指令：relay:100 / get / rule:list …") },
                    singleLine = true,
                    modifier = Modifier.fillMaxWidth(),
                )
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(
                        onClick = {
                            val c = cmdText.value.trim()
                            // send() 返回 false = **根本没发出去**（未连接/发布异常）。
                            // 必须立刻说清楚，而不是记一笔在途、让用户等 4 秒超时。
                            if (c.isNotEmpty() && mqtt?.send(c) != true) addCmdLog("✗ 未发送：$c")
                        },
                        enabled = mqttConnected.value,
                    ) { Text("发送") }
                    OutlinedButton(
                        onClick = { mqtt?.send("get") },
                        enabled = mqttConnected.value,
                    ) { Text("拉取遥测") }
                }

                if (mqttLog.isNotEmpty()) {
                    Text("过程：", style = MaterialTheme.typography.bodySmall)
                    mqttLog.forEach { line ->
                        Text(
                            line,
                            style = MaterialTheme.typography.bodySmall,
                            fontFamily = FontFamily.Monospace,
                            color = when {
                                line.startsWith("✓") -> Color(0xFF2E7D32)
                                line.startsWith("✗") || line.startsWith("⏱") -> Color(0xFFB00020)
                                line.startsWith("⚠") -> Color(0xFFEF6C00)
                                else -> MaterialTheme.colorScheme.onSurface
                            },
                        )
                    }
                }
            }
        }
    }

    @Composable
    private fun TelemetryCard(t: Telemetry, title: String = "实时数据") {
        Card(modifier = Modifier.fillMaxWidth()) {
            Column(
                modifier = Modifier.padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(6.dp),
            ) {
                Text(title, style = MaterialTheme.typography.titleMedium)
                ValueRow("空气温度", t.airTempC, "℃", t.airOk)
                ValueRow("空气湿度", t.airRh, "%", t.airOk)
                ValueRow("水温", t.waterTempC, "℃", t.waterOk)
                Text("继电器 ${bits(t.relayMask)}    输入 ${bits(t.inputMask)}",
                    fontFamily = FontFamily.Monospace,
                    style = MaterialTheme.typography.bodyMedium)
                Text("WiFi 信号 ${t.rssi} dBm    设备运行 ${t.uptimeSec} 秒",
                    style = MaterialTheme.typography.bodySmall)
            }
        }
    }

    @Composable
    private fun ValueRow(label: String, value: Double?, unit: String, valid: Boolean) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
        ) {
            Text(label, style = MaterialTheme.typography.bodyLarge)
            Text(
                // 无效读数显示「无效」而不是 0.0：设备端发的是 null，
                // 显示成 0.0 会误导（固件当初正是为此修掉了哨兵值）
                text = if (valid && value != null) "%.1f %s".format(value, unit) else "无效",
                style = MaterialTheme.typography.bodyLarge,
                color = if (valid && value != null) MaterialTheme.colorScheme.onSurface
                else Color(0xFFB00020),
            )
        }
    }

    private fun bits(mask: Int): String =
        "${(mask shr 0) and 1}${(mask shr 1) and 1}${(mask shr 2) and 1}"
}
