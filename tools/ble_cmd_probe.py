#!/usr/bin/env python3
"""向设备的**指令特征**（f0a00003）写入一条指令并打印回执。

与 ble_probe.py 的分工
----------------------
    ble_probe.py  写 → **配网特征** f0a00005
    本脚本        写 → **指令特征** f0a00003

为什么必须单独有这个脚本
------------------------
指令特征走的是 `cmd_execute(..., allow_provisioning=true)`，也就是
**往指令特征写 `wifi:pass:<密码>` 同样合法**。这条路径曾经会把密码原文
打进串口日志（缺陷 #2），而 ble_probe.py 只写配网特征，覆盖不到它。
本脚本用于回归验证「密码绝不出现在任何日志里」。

⚠️ 固件 NimBLE 配置为 MAX_CONNECTIONS=1：运行前请先断开手机上的 APP，
   否则设备已被占用，连不上。

⚠️ 往指令特征写 `wifi:pass:` 会把密码送进设备的**待用凭据**（仅 RAM）。
   本脚本不会发送 `wifi:connect`，因此不会落盘、不会改变当前连接；
   重启即清除。默认参数是一条只读指令，不带任何副作用。

用法：
    python ble_cmd_probe.py                          # 默认发只读的 status
    python ble_cmd_probe.py "wifi:pass:SENTINEL-PW"  # 验证密码不落日志
    python ble_cmd_probe.py --no-pair "status"       # 不主动配对
"""

import argparse
import asyncio
import sys

from bleak import BleakClient, BleakScanner

DEVICE_PREFIX = "SmartFarm-"
CHR_COMMAND = "f0a00003-8b3c-4d5e-9f10-2a3b4c5d6e7f"


def mask(cmd: str) -> str:
    """本地也做一次遮蔽，免得脚本自己的输出把密码打到终端记录里。"""
    low = cmd.lower()
    i = low.find("wifi:pass:")
    if i < 0:
        return cmd
    return f"{cmd[:i]}wifi:pass:<{len(cmd) - i - len('wifi:pass:')} bytes hidden>"


def on_notify(_sender, data: bytearray) -> None:
    print(f"  [ACK] {data.decode('utf-8', 'replace')}", flush=True)


async def main() -> int:
    ap = argparse.ArgumentParser(description="向指令特征写入并观察回执")
    ap.add_argument("command", nargs="?", default="status", help="要写入的指令文本")
    ap.add_argument("--no-pair", action="store_true", help="不主动配对")
    ap.add_argument("--listen", type=float, default=8.0, help="写完后收听秒数")
    args = ap.parse_args()

    print(f"指令（已遮蔽）: {mask(args.command)}", flush=True)
    print(f"载荷长度      : {len(args.command.encode())} 字节", flush=True)

    print("扫描设备…", flush=True)
    device = await BleakScanner.find_device_by_filter(
        lambda d, ad: (d.name or ad.local_name or "").startswith(DEVICE_PREFIX),
        timeout=20.0,
    )
    if device is None:
        print("未找到 SmartFarm 设备（手机 APP 是否仍连着？MAX_CONNECTIONS=1）")
        return 1
    print(f"找到 {device.name}  {device.address}", flush=True)

    async with BleakClient(device, timeout=25.0) as client:
        print(f"已连接 = {client.is_connected}", flush=True)

        # 写指令特征需要加密链路，先配对
        if not args.no_pair and hasattr(client, "pair"):
            try:
                print("尝试配对（已配对应直接成功）…", flush=True)
                await client.pair()
                print("配对成功", flush=True)
            except Exception as exc:  # noqa: BLE001
                print(f"配对未成功：{type(exc).__name__}: {exc}", flush=True)

        try:
            await client.start_notify(CHR_COMMAND, on_notify)
            print("已订阅指令特征回执", flush=True)
        except Exception as exc:  # noqa: BLE001
            print(f"订阅失败：{type(exc).__name__}: {exc}", flush=True)

        print("写入指令特征 …", flush=True)
        try:
            await client.write_gatt_char(CHR_COMMAND,
                                        args.command.encode("utf-8"), response=True)
            print("★ 写入调用返回成功", flush=True)
        except Exception as exc:  # noqa: BLE001
            print(f"★ 写入失败：{type(exc).__name__}: {exc}", flush=True)

        print(f"等待回执 {args.listen:g} 秒…", flush=True)
        await asyncio.sleep(args.listen)

    print("已断开", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
