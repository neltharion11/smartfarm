"""
通过 BLE 给设备下发 WiFi 凭据（正确凭据路径的端到端验收）。

凭据从固件的 `main/secrets.h` 读取 —— **脚本自己解析，不经过命令行参数**，
避免密码出现在命令历史、进程列表或对话记录里。输出里也只打印长度。

⚠️ 固件 MAX_CONNECTIONS=1：运行前需断开手机上的 APP。

用法：python ble_provision.py [--status-only]
"""
import asyncio
import re
import sys
from pathlib import Path

from bleak import BleakClient, BleakScanner

DEVICE_PREFIX = "SmartFarm-"
CHR_TELEMETRY = "f0a00002-8b3c-4d5e-9f10-2a3b4c5d6e7f"
CHR_WIFI = "f0a00005-8b3c-4d5e-9f10-2a3b4c5d6e7f"

SECRETS = Path(__file__).resolve().parent.parent / "main" / "secrets.h"


def read_secrets():
    """从 secrets.h 取 SSID / 密码。只返回内容，调用方负责不打印。"""
    if not SECRETS.exists():
        print(f"找不到 {SECRETS}")
        return None, None
    text = SECRETS.read_text(encoding="utf-8", errors="replace")
    ssid = pw = None
    for m in re.finditer(r'#define\s+(\w+)\s+"([^"]*)"', text):
        name, value = m.group(1), m.group(2)
        if "WIFI_SSID" in name.upper():
            ssid = value
        elif "WIFI_PASS" in name.upper():
            pw = value
    return ssid, pw


def on_notify(sender, data: bytearray):
    print(f"  [回执] {data.decode('utf-8', 'replace')}", flush=True)


async def main() -> int:
    status_only = "--status-only" in sys.argv

    ssid, password = read_secrets()
    if ssid is None:
        print("未能从 secrets.h 解析出 SSID")
        return 1
    print(f"已从 secrets.h 读到凭据：SSID={ssid}  密码长度={len(password or '')}", flush=True)

    print("扫描设备…", flush=True)
    device = await BleakScanner.find_device_by_filter(
        lambda d, ad: (d.name or ad.local_name or "").startswith(DEVICE_PREFIX), timeout=20.0)
    if device is None:
        print("未找到设备")
        return 1
    print(f"找到 {device.name}", flush=True)

    async with BleakClient(device, timeout=25.0) as client:
        print(f"已连接 = {client.is_connected}", flush=True)
        try:
            print(f"MTU = {client.mtu_size}", flush=True)
        except Exception:
            pass
        try:
            await client.pair()
            print("配对成功", flush=True)
        except Exception as e:
            print(f"配对：{type(e).__name__}: {e}", flush=True)

        await client.start_notify(CHR_WIFI, on_notify)
        await client.start_notify(CHR_TELEMETRY, on_notify)

        if not status_only:
            cmds = [
                f"wifi:ssid:{ssid}",
                f"wifi:pass:{password}",
                "wifi:connect",
            ]
            for c in cmds:
                shown = "wifi:pass:***" if c.startswith("wifi:pass:") else c
                print(f"→ {shown}", flush=True)
                await client.write_gatt_char(CHR_WIFI, c.encode("utf-8"), response=True)
                await asyncio.sleep(1.5)

        print("→ wifi:status", flush=True)
        await client.write_gatt_char(CHR_WIFI, b"wifi:status", response=True)
        await asyncio.sleep(10)

    print("完成", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
