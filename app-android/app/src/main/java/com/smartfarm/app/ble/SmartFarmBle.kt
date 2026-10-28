package com.smartfarm.app.ble

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothStatusCodes
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import androidx.core.content.ContextCompat

/**
 * SmartFarm BLE 客户端：扫描 → 连接 → 协商 MTU → 订阅遥测/配网 → 下发配网指令。
 *
 * 设计要点（每一条都对应一个真实会踩的坑）：
 *
 * 1. **必须请求 MTU 512**。默认 MTU 23 时单次通知只有 20 字节，
 *    110 字节的遥测 JSON 会被截断成 `{"air_ok":1,"air_t":` —— 这正是
 *    "看着连上了、内容却不对"的经典症状。
 *
 * 2. **仍然实现分片重组兜底**：设备在未协商 MTU 时会分片并加 `n/t|` 前缀。
 *
 * 3. **GATT 操作必须串行**：Android 的 GATT 同时只允许一个未完成操作
 *    （写描述符/写特征）。并发发第二个会被静默丢弃或返回失败。
 *    所以订阅两个特征的 CCCD、以及下发多条配网指令，都用队列串起来。
 *
 * 4. **写配网特征会触发配对**：固件给它标了 `WRITE_ENC`，未加密链路写不进去。
 *    正确做法是**先配对、配对完成后再重试写入**——直接写会拿到
 *    `GATT_INSUFFICIENT_AUTHENTICATION`，而 Android 不保证自动重试。
 *
 * 5. **遥测不需要配对**（固件里它是开放的），所以不配网也能看数据。
 */
@SuppressLint("MissingPermission")
class SmartFarmBle(
    private val context: Context,
    private val listener: Listener,
) {
    interface Listener {
        /** 过程状态（界面显示"正在扫描/已连接/MTU=..."） */
        fun onStatus(text: String)

        /** 收到一条完整遥测 */
        fun onTelemetry(t: Telemetry, mtu: Int, fragments: Int)

        /** 收到一条配网过程消息 */
        fun onProvision(ev: ProvEvent)

        /**
         * 收到一条**指令特征**的回执（分片前缀已剥离的完整 JSON 文本）。
         *
         * 这里**不解析**：判定规则（尤其"缺失 ok 字段 ≠ 成功"）统一由
         * `AckTracker.ackVerdict` 提供，与 MQTT 通道共用同一套已单测的逻辑，
         * 避免两条通道对同一条回执得出不同结论。
         */
        fun onCommandAck(raw: String)

        /** 蓝牙链路状态变化——通道路由靠它判断"现在该走哪条路"。 */
        fun onLinkState(connected: Boolean, ready: Boolean)

        /** 错误/异常情况 */
        fun onError(text: String)
    }

    companion object {
        private const val TAG = "SmartFarmBle"
        private const val SCAN_TIMEOUT_MS = 15_000L
    }

    private val handler = Handler(Looper.getMainLooper())

    /**
     * 遥测的分片重组器。
     *
     * ⚠️ **每个特征必须用独立的重组器**：设备的通知分片带 `n/t|` 前缀，
     * 若两个特征共用一个实例，交错到达的分片会互相污染，拼出乱七八糟的 JSON。
     */
    private val assembler = TelemetryAssembler()

    /** 配网回执的重组器（独立实例，理由同上）。 */
    private val wifiAssembler = TelemetryAssembler()

    /** 指令特征回执的重组器（独立实例，理由同上）。 */
    private val cmdAssembler = TelemetryAssembler()

    private var gatt: BluetoothGatt? = null
    private var device: BluetoothDevice? = null
    private var scanning = false
    private var mtu = 23                    // BLE 规范默认值；不是 0
    private var ready = false               // 两个 CCCD 都写完才算就绪

    private var telemetryChar: BluetoothGattCharacteristic? = null
    private var wifiChar: BluetoothGattCharacteristic? = null
    private var cmdChar: BluetoothGattCharacteristic? = null

    /** 待开启通知的特征（串行处理，GATT 一次只能有一个操作） */
    private val cccdQueue = ArrayDeque<BluetoothGattCharacteristic>()

    /**
     * 待下发的指令（串行处理）。
     *
     * ⚠️ 元素必须带上"写到哪个特征"：**指令特征**与**配网特征**都要写，
     * 而 GATT 同时只允许一个未完成操作，所以两者共用同一个队列串行下发。
     * 早期只支持配网特征，加了指令控制后若仍用 `ArrayDeque<String>`，
     * 就会出现"指令被写到配网特征上"这种静默错投。
     */
    private data class Write(val toCommand: Boolean, val text: String)
    private val writeQueue = ArrayDeque<Write>()

    /**
     * 是否有 GATT 写操作正在进行。
     *
     * ⚠️ 必须自己跟踪：Android 的 GATT **同时只允许一个未完成操作**，
     * 上一个还没回调就又发一个，`writeCharacteristic()` 会直接返回 false。
     * 那不是"失败"——指令还在队列里，等回调来了自然会发。
     * 早期没跟踪这个，导致界面上出现两条**假报错**（"写请求未能发起"），
     * 把真实状态盖住了。
     */
    private var writeInFlight = false

    private var bondReceiverRegistered = false

    private val adapter: BluetoothAdapter?
        get() = (context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager)?.adapter

    val isScanning: Boolean get() = scanning
    val isReady: Boolean get() = ready

    // ------------------------------------------------------------ 配对事件

    /** 配对状态变化：配对成功后要把排队中的写入继续下去。 */
    private val bondReceiver = object : BroadcastReceiver() {
        override fun onReceive(ctx: Context, intent: Intent) {
            if (intent.action != BluetoothDevice.ACTION_BOND_STATE_CHANGED) return
            @Suppress("DEPRECATION")
            val dev = intent.getParcelableExtra<BluetoothDevice>(BluetoothDevice.EXTRA_DEVICE)
            if (dev == null || dev.address != device?.address) return

            when (intent.getIntExtra(BluetoothDevice.EXTRA_BOND_STATE, BluetoothDevice.BOND_NONE)) {
                BluetoothDevice.BOND_BONDING ->
                    listener.onStatus("正在配对：请在手机上输入设备显示的 6 位 Passkey…")
                BluetoothDevice.BOND_BONDED -> {
                    listener.onStatus("配对完成，继续下发配网指令")
                    tryWriteNext()
                }
                else -> listener.onStatus("配对未完成")
            }
        }
    }

    private fun registerBondReceiver() {
        if (bondReceiverRegistered) return
        ContextCompat.registerReceiver(
            context,
            bondReceiver,
            IntentFilter(BluetoothDevice.ACTION_BOND_STATE_CHANGED),
            ContextCompat.RECEIVER_NOT_EXPORTED,
        )
        bondReceiverRegistered = true
    }

    // ------------------------------------------------------------ 扫描

    fun startScan() {
        val a = adapter
        if (a == null) { listener.onError("本机没有蓝牙适配器"); return }
        if (!a.isEnabled) { listener.onError("蓝牙未开启，请先在系统设置里打开蓝牙"); return }

        closeGatt()
        val scanner = a.bluetoothLeScanner
        if (scanner == null) { listener.onError("无法获取 BLE 扫描器"); return }

        // 不设过滤器，在回调里按设备名前缀匹配：
        // 固件广播包里只有名字（31 字节放不下服务 UUID），按 UUID 过滤会扫不到。
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()

        scanning = true
        listener.onStatus("正在扫描 ${BleConst.DEVICE_NAME_PREFIX}* …")
        scanner.startScan(null, settings, scanCallback)

        handler.postDelayed({
            if (scanning) {
                stopScan()
                listener.onError("${SCAN_TIMEOUT_MS / 1000} 秒内没找到以 " +
                        "\"${BleConst.DEVICE_NAME_PREFIX}\" 开头的设备。\n" +
                        "请确认设备已上电并在广播。")
            }
        }, SCAN_TIMEOUT_MS)
    }

    fun stopScan() {
        if (!scanning) return
        scanning = false
        adapter?.bluetoothLeScanner?.stopScan(scanCallback)
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val name = result.device.name ?: result.scanRecord?.deviceName
            if (name != null && name.startsWith(BleConst.DEVICE_NAME_PREFIX)) {
                stopScan()
                listener.onStatus("发现 $name（RSSI ${result.rssi}），正在连接…")
                connect(result.device)
            }
        }

        override fun onScanFailed(errorCode: Int) {
            scanning = false
            listener.onError("扫描失败，错误码 $errorCode")
        }
    }

    // ------------------------------------------------------------ 连接

    private fun connect(dev: BluetoothDevice) {
        device = dev
        registerBondReceiver()
        gatt = dev.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
    }

    private val gattCallback = object : BluetoothGattCallback() {

        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            when (newState) {
                BluetoothProfile.STATE_CONNECTED -> {
                    gatt = g
                    ready = false
                    listener.onLinkState(connected = true, ready = false)
                    listener.onStatus("已连接，正在请求 MTU ${BleConst.REQUEST_MTU}…")
                    g.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH)
                    if (!g.requestMtu(BleConst.REQUEST_MTU)) {
                        listener.onStatus("MTU 请求未能发起，直接发现服务")
                        g.discoverServices()
                    }
                }

                BluetoothProfile.STATE_DISCONNECTED -> {
                    ready = false
                    writeInFlight = false
                    telemetryChar = null
                    wifiChar = null
                    cmdChar = null
                    listener.onLinkState(connected = false, ready = false)
                    listener.onStatus("已断开（status=$status）")
                }
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, newMtu: Int, status: Int) {
            mtu = newMtu
            // MTU 是**协商结果**，可能被对端压低；显示在界面上便于一眼看出
            // 是否真的拿到了大 MTU（拿不到就会走分片兜底）
            listener.onStatus("MTU = $newMtu（单次通知上限 ${newMtu - 3} 字节），正在发现服务…")
            g.discoverServices()
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                listener.onError("服务发现失败，status=$status")
                return
            }
            val service = g.getService(BleConst.SERVICE)
            if (service == null) {
                listener.onError("设备上没有找到 SmartFarm 服务（UUID 不匹配？）")
                return
            }

            telemetryChar = service.getCharacteristic(BleConst.CHR_TELEMETRY)
            wifiChar = service.getCharacteristic(BleConst.CHR_WIFI)
            cmdChar = service.getCharacteristic(BleConst.CHR_COMMAND)

            if (telemetryChar == null) {
                listener.onError("服务里没有遥测特征")
                return
            }

            // 串行开启通知：GATT 同时只能有一个未完成操作。
            // ⚠️ **指令特征也要开通知**：固件把回执从**同一特征**推回来
            // （Write + Notify 配对使用），不开通知就永远收不到回执，
            // 表现为"指令发出去了但没有任何结果"。
            cccdQueue.clear()
            cccdQueue.addLast(telemetryChar!!)
            if (wifiChar != null) cccdQueue.addLast(wifiChar!!)
            if (cmdChar != null) cccdQueue.addLast(cmdChar!!)
            nextCccd(g)
        }

        @Deprecated("Deprecated in Java")
        override fun onDescriptorWrite(
            g: BluetoothGatt,
            descriptor: BluetoothGattDescriptor,
            status: Int,
        ) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                listener.onError("开启通知失败（CCCD 写 status=$status）")
                return
            }
            if (cccdQueue.isEmpty()) {
                ready = true
                listener.onStatus("已就绪：可读遥测、可下发指令与配网")
                listener.onLinkState(connected = true, ready = true)
                tryWriteNext()
            } else {
                nextCccd(g)
            }
        }

        @Deprecated("Deprecated in Java")
        override fun onCharacteristicWrite(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int,
        ) {
            Log.i(TAG, "onCharacteristicWrite uuid=${characteristic.uuid} status=$status")
            if (characteristic.uuid != BleConst.CHR_WIFI &&
                characteristic.uuid != BleConst.CHR_COMMAND
            ) return

            writeInFlight = false

            if (status == BluetoothGatt.GATT_SUCCESS) {
                tryWriteNext()
                return
            }

            // 5 = INSUFFICIENT_AUTHENTICATION，15 = INSUFFICIENT_ENCRYPTION
            // 都是"链路还没加密"——先配对，配对完成后重试（指令仍在队列里）
            if (status == 5 || status == 15) {
                val dev = device
                if (dev != null && dev.bondState != BluetoothDevice.BOND_BONDED) {
                    listener.onStatus("写入被拒（链路未加密），正在发起配对…")
                    dev.createBond()
                } else {
                    // 已配对却被拒：多半是**旧绑定已失效**（固件重烧过、密钥不匹配）。
                    // 光提示没用，要告诉用户怎么修。
                    listener.onError(
                        "写入被拒（status=$status），但手机显示已配对。\n" +
                                "多半是旧配对已失效（设备固件重烧过）。\n" +
                                "请在系统蓝牙设置里「忽略/取消配对」SmartFarm 设备，" +
                                "然后重新连接——应用会重新发起配对。"
                    )
                }
            } else {
                listener.onError("配网指令写入失败，status=$status")
            }
        }

        @Deprecated("Deprecated in Java")
        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
        ) {
            @Suppress("DEPRECATION")
            handleNotification(characteristic.uuid, characteristic.value ?: return)
        }

        override fun onCharacteristicChanged(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray,
        ) {
            handleNotification(characteristic.uuid, value)
        }
    }

    private fun nextCccd(g: BluetoothGatt) {
        val chr = cccdQueue.removeFirstOrNull()
        if (chr == null) {
            ready = true
            listener.onStatus("已就绪：可读遥测、可下发配网")
            tryWriteNext()
            return
        }
        g.setCharacteristicNotification(chr, true)
        val cccd = chr.getDescriptor(BleConst.CCCD)
        if (cccd == null) {
            listener.onError("特征 ${chr.uuid} 没有 CCCD")
            nextCccd(g)
            return
        }
        @Suppress("DEPRECATION")
        cccd.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
        @Suppress("DEPRECATION")
        if (!g.writeDescriptor(cccd)) {
            listener.onError("CCCD 写请求未能发起")
        }
    }

    private fun handleNotification(uuid: java.util.UUID, bytes: ByteArray) {
        val text = String(bytes, Charsets.UTF_8)
        when (uuid) {
            BleConst.CHR_TELEMETRY -> {
                val complete = assembler.feed(text) ?: return   // 还缺片
                val t = Telemetry.parse(complete)
                if (t == null) {
                    listener.onError("遥测 JSON 解析失败：${complete.take(80)}")
                } else {
                    listener.onTelemetry(t, mtu, assembler.fragmentCount)
                }
            }

            BleConst.CHR_WIFI -> {
                // ⚠️ 配网回执同样带 `n/t|` 分片前缀（设备侧走的是同一个分片函数）。
                // 早期版本直接把原始字节丢给解析器，结果界面上显示成
                // `? 1/1|{"ack":"wifi:ssid",...}` —— JSON 前缀没剥掉，解析必然失败。
                val complete = wifiAssembler.feed(text) ?: return
                listener.onProvision(WifiProvision.parse(complete))
            }

            BleConst.CHR_COMMAND -> {
                // 指令回执同样分片（同一个固件分片函数），必须用**独立**重组器
                val complete = cmdAssembler.feed(text) ?: return
                listener.onCommandAck(complete)
            }
        }
    }

    // ------------------------------------------------------------ 配网写入

    /**
     * 入队一条配网指令（自动串行下发，并在需要时先配对）。
     * 仅用于 `wifi:*`：写的是**配网特征**。
     */
    fun writeWifiCommand(text: String) {
        writeQueue.addLast(Write(toCommand = false, text = text))
        tryWriteNext()
    }

    /**
     * 入队一条**控制指令**（走**指令特征** `…0003`）。
     *
     * 与配网分开的原因：两者是不同的特征，且指令特征的 `allow_provisioning=true`，
     * 语义上更宽；混用会让"这条到底写到哪去了"变得不可推理。
     */
    fun writeCommand(text: String) {
        writeQueue.addLast(Write(toCommand = true, text = text))
        tryWriteNext()
    }

    /** 队列里还剩几条没发。 */
    fun pendingWrites(): Int = writeQueue.size

    private fun tryWriteNext() {
        if (!ready) return
        if (writeInFlight) return          // 上一个写还没回调，等它
        val g = gatt ?: return
        val item = writeQueue.firstOrNull() ?: return
        val chr = (if (item.toCommand) cmdChar else wifiChar) ?: run {
            listener.onError(if (item.toCommand) "设备没有指令特征" else "设备没有配网特征")
            return
        }
        val cmd = item.text

        // 写配网特征需要加密链路。先确认已配对，避免先撞一次
        // GATT_INSUFFICIENT_AUTHENTICATION 再补救（那样日志会很乱）。
        val dev = device
        if (dev != null && dev.bondState != BluetoothDevice.BOND_BONDED) {
            listener.onStatus("配网需要先配对，正在发起…")
            dev.createBond()
            return
        }

        val payload = cmd.toByteArray(Charsets.UTF_8)
        // 单次写入上限也是 MTU-3；超了会失败（长写需要 prepare-write，固件未支持）
        if (payload.size > mtu - 3) {
            writeQueue.removeFirst()
            listener.onError("指令过长（${payload.size} 字节 > MTU-3=${mtu - 3}），已取消")
            return
        }

        @Suppress("DEPRECATION")
        chr.value = payload
        @Suppress("DEPRECATION")
        chr.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT   // 固件是"带响应写"

        // ⚠️ API 33(TIRAMISU) 起 `setValue()` + `writeCharacteristic(c)` 这一对已弃用，
        // 在较新系统上可能**静默失效**（返回成功但 ATT 层没发出去、也没有回调）。
        // 必须用带 value 的新重载；它返回状态码而不是布尔值。
        val ok = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            val st = g.writeCharacteristic(
                chr, payload, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
            )
            st == BluetoothStatusCodes.SUCCESS
        } else {
            @Suppress("DEPRECATION")
            g.writeCharacteristic(chr)
        }

        Log.i(TAG, "write ${maskSecret(cmd)} len=${payload.size} ok=$ok inFlight=$writeInFlight")

        if (ok) {
            writeInFlight = true
            writeQueue.removeFirst()
            listener.onStatus("已下发：${maskSecret(cmd)}")
        } else {
            // 极少数情况（栈内部忙）会返回 false。指令**留在队列**，
            // 等下一次回调再试——不要报成失败，那会误导。
            Log.w(TAG, "writeCharacteristic returned false, will retry: ${maskSecret(cmd)}")
        }
    }

    /** 日志/界面里显示指令时**屏蔽密码**，避免它出现在日志里。 */
    private fun maskSecret(cmd: String): String =
        if (cmd.startsWith("wifi:pass:")) "wifi:pass:***" else cmd

    // ------------------------------------------------------------ 收尾

    fun closeGatt() {
        stopScan()
        if (bondReceiverRegistered) {
            try { context.unregisterReceiver(bondReceiver) } catch (e: Exception) {
                Log.w(TAG, "unregisterReceiver: ${e.message}")
            }
            bondReceiverRegistered = false
        }
        gatt?.let {
            try { it.disconnect() } catch (e: Exception) { Log.w(TAG, "disconnect: ${e.message}") }
            try { it.close() } catch (e: Exception) { Log.w(TAG, "close: ${e.message}") }
        }
        gatt = null
        device = null
        telemetryChar = null
        wifiChar = null
        cmdChar = null
        cccdQueue.clear()
        writeQueue.clear()
        ready = false
        writeInFlight = false
        listener.onLinkState(connected = false, ready = false)
        handler.removeCallbacksAndMessages(null)
    }
}
