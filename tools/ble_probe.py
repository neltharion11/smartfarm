"""
用电脑作为**独立的 BLE 中心设备**去写 SmartFarm 的配网特征。

目的：区分「手机蓝牙栈的问题」和「固件/设备的问题」。
如果电脑能写进去并拿到回执，说明设备侧是好的，问题在手机（荣耀栈或 APP）；
如果电脑也写不进去，说明问题在固件/设备侧。

⚠️ 固件的 NimBLE 配置是 MAX_CONNECTIONS=1，所以跑这个脚本前
   必须先断开手机上的 APP（否则设备已被占用，连不上）。

用法：python ble_probe.py
"""
import asyncio
import sys

from bleak import BleakClient, BleakScanner

DEVICE_PREFIX = "SmartFarm-"
CHR_TELEMETRY = "f0a00002-8b3c-4d5e-9f10-2a3b4c5d6e7f"
CHR_WIFI = "f0a00005-8b3c-4d5e-9f10-2a3b4c5d6e7f"


def on_notify(sender, data: bytearray):
    text = data.decode("utf-8", "replace")
    print(f"  [NOTIFY] {text}", flush=True)


async def main() -> int:
    print("扫描设备…", flush=True)
    device = await BleakScanner.find_device_by_filter(
        lambda d, ad: (d.name or ad.local_name or "").startswith(DEVICE_PREFIX),
        timeout=20.0,
    )
    if device is None:
        print("未找到 SmartFarm 设备")
        return 1
    print(f"找到 {device.name}  {device.address}", flush=True)

    async with BleakClient(device, timeout=25.0) as client:
        print(f"已连接 = {client.is_connected}", flush=True)

        try:
            print(f"MTU = {client.mtu_size}", flush=True)
        except Exception as e:                      # Windows 上不一定支持
            print(f"MTU 读取不可用：{e}", flush=True)

        # 写配网特征需要加密链路；先尝试配对
        if hasattr(client, "pair"):
            try:
                print("尝试配对（若已配对应当直接成功）…", flush=True)
                await client.pair()
                print("配对成功", flush=True)
            except Exception as e:
                print(f"配对未成功：{type(e).__name__}: {e}", flush=True)

        # 订阅遥测与配网回执
        try:
            await client.start_notify(CHR_TELEMETRY, on_notify)
            print("已订阅遥测", flush=True)
        except Exception as e:
            print(f"订阅遥测失败：{e}", flush=True)
        try:
            await client.start_notify(CHR_WIFI, on_notify)
            print("已订阅配网回执", flush=True)
        except Exception as e:
            print(f"订阅配网回执失败：{e}", flush=True)

        # 关键一步：写一条最简指令
        print("写 wifi:status …", flush=True)
        try:
            await client.write_gatt_char(CHR_WIFI, b"wifi:status", response=True)
            print("★ 写入调用返回成功", flush=True)
        except Exception as e:
            print(f"★ 写入失败：{type(e).__name__}: {e}", flush=True)

        print("等待回执 8 秒…", flush=True)
        await asyncio.sleep(8)

    print("已断开", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
