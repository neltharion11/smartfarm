#!/usr/bin/env python3
"""MQTT 指令与观测工具（电脑侧，只用标准库）

用途
----
在**不碰设备、不用手机**的情况下，直接从电脑验证整条 MQTT 控制链路：

    python tools/mqtt_cmd.py relay:100     # 发控制指令并看回执
    python tools/mqtt_cmd.py get           # 请求立即上报
    python tools/mqtt_cmd.py status
    python tools/mqtt_cmd.py --watch 20    # 只订阅观察 20 秒，不发指令

它会订阅 `farm/<node>/#`，把收到的每条消息（回执 / 遥测 / 在线状态）打印出来。
默认在结束时发 `relay:000` 把所有继电器复位，避免测试后留着负载通电
（用 --no-restore 关闭）。

为什么不用 paho-mqtt
--------------------
IDF 6.0.2 已经把 cJSON、esp-mqtt 都移出核心，本机也没装 paho。
手写 MQTT 3.1.1 报文只需几十行，而且能顺带证明"我们的报文构造是对的"。
凭据从 main/secrets.h 读，**不打印密码**。

注意：本工具**不能**发配网指令（wifi:*）——固件侧已把配网限制为仅 BLE 可用。
"""

import argparse
import os
import re
import socket
import ssl
import struct
import sys
import time

DEFAULT_SECRETS = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "main", "secrets.h"
)

# 与 net_config.h 的默认值保持一致
DEFAULT_TOPIC_ROOT = "farm/node1"

CONNECT_TIMEOUT_S = 20

PKT_CONNACK = 0x20
PKT_PUBLISH = 0x30
PKT_PUBACK = 0x40
PKT_SUBACK = 0x90
PKT_PINGRESP = 0xD0


# ---------------------------------------------------------------- secrets

def load_secrets(path):
    with open(path, encoding="utf-8") as f:
        src = f.read()

    def get(name):
        m = re.search(r"#define\s+" + name + r'\s+"([^"]*)"', src)
        return m.group(1) if m else None

    return {
        "uri": get("SECRET_MQTT_URI"),
        "user": get("SECRET_MQTT_USERNAME"),
        "password": get("SECRET_MQTT_PASSWORD"),
    }


# ---------------------------------------------------------------- encoding

def mqtt_str(s):
    b = s.encode()
    return len(b).to_bytes(2, "big") + b


def enc_remaining_length(n):
    """MQTT 剩余长度的变长编码（每字节 7 位，最高位为续接标志）。"""
    out = bytearray()
    while True:
        byte = n % 128
        n //= 128
        if n > 0:
            byte |= 0x80
        out.append(byte)
        if n == 0:
            return bytes(out)


def build_connect(client_id, user, password, keepalive=60):
    flags = 0x02
    if user:
        flags |= 0x80
    if password:
        flags |= 0x40
    vh = mqtt_str("MQTT") + bytes([4, flags]) + keepalive.to_bytes(2, "big")
    payload = mqtt_str(client_id)
    if user:
        payload += mqtt_str(user)
    if password:
        payload += mqtt_str(password)
    body = vh + payload
    return bytes([0x10]) + enc_remaining_length(len(body)) + body


def build_subscribe(packet_id, topic, qos=0):
    body = packet_id.to_bytes(2, "big") + mqtt_str(topic) + bytes([qos])
    return bytes([0x82]) + enc_remaining_length(len(body)) + body


def build_publish(topic, payload, qos=1, packet_id=1):
    """QoS 1 需要在主题之后带 2 字节 packet id；QoS 0 不带。

    ⚠️ 默认用 **QoS 1**：QoS 0 是"尽力而为、不保证送达"，链路稍有抖动
    指令就会**静默丢失**（实测踩过：回执延迟数秒、甚至完全丢失，
    而设备其实活着、遥测一直在流）。
    注意 MQTT 的投递等级取"发布 QoS 与订阅 QoS 的较小值"——
    即使设备订阅了 QoS 1，只要我们发布用 QoS 0，实际仍会降级成 QoS 0。"""
    body = mqtt_str(topic)
    if qos > 0:
        body += packet_id.to_bytes(2, "big")
    body += payload.encode()
    return bytes([0x30 | (qos << 1)]) + enc_remaining_length(len(body)) + body


# ---------------------------------------------------------------- decoding

def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("connection closed while reading")
        buf += chunk
    return buf


def recv_remaining_length(sock):
    value = 0
    multiplier = 1
    for _ in range(4):
        byte = recv_exact(sock, 1)[0]
        value += (byte & 0x7F) * multiplier
        if not (byte & 0x80):
            return value
        multiplier *= 128
    raise ValueError("malformed remaining length")


def recv_packet(sock):
    """返回 (packet_type, body)。"""
    header = recv_exact(sock, 1)[0]
    length = recv_remaining_length(sock)
    body = recv_exact(sock, length) if length else b""
    return header & 0xF0, body


def parse_publish(body):
    tlen = struct.unpack(">H", body[:2])[0]
    topic = body[2:2 + tlen].decode(errors="replace")
    payload = body[2 + tlen:].decode(errors="replace")
    return topic, payload


# ---------------------------------------------------------------- main

def parse_args(argv):
    p = argparse.ArgumentParser(description="MQTT 指令与观测工具（电脑侧阳性对照）")
    p.add_argument("commands", nargs="*", default=["status"],
                   help="要发布的指令，可给多条（**共用同一条连接**）："
                        "relay:100 / get / status / rule:list ...")
    p.add_argument("--secrets", default=DEFAULT_SECRETS, help="secrets.h 路径")
    p.add_argument("--topic-root", default=DEFAULT_TOPIC_ROOT,
                   help=f"主题前缀（默认 {DEFAULT_TOPIC_ROOT}）")
    p.add_argument("--watch", type=int, default=6,
                   help="最后一条指令之后继续收听的秒数（默认 6）")
    p.add_argument("--gap", type=float, default=1.5,
                   help="多条指令之间的间隔秒数（默认 1.5）")
    p.add_argument("--no-restore", action="store_true",
                   help="结束后不发送 relay:000 复位")
    return p.parse_args(argv)


def main():
    args = parse_args(sys.argv[1:])
    commands = list(args.commands) if args.commands else ["status"]

    if not os.path.exists(args.secrets):
        print(f"secrets.h not found: {args.secrets}")
        return 2

    cfg = load_secrets(args.secrets)
    if not cfg["uri"]:
        print("SECRET_MQTT_URI is empty")
        return 2

    uri = cfg["uri"] if "//" in cfg["uri"] else "mqtts://" + cfg["uri"]
    host = uri.split("//", 1)[1].split(":", 1)[0]
    port = int(uri.rsplit(":", 1)[1])

    cmd_topic = f"{args.topic_root}/cmd"
    sub_topic = f"{args.topic_root}/#"

    print(f"broker   : {host}:{port}")
    print(f"username : {cfg['user']!r}")
    print(f"password : <hidden> (length {len(cfg['password'] or '')})")
    print(f"subscribe: {sub_topic}")
    print(f"commands : {len(commands)} 条，共用同一条连接 -> {cmd_topic}")

    ctx = ssl.create_default_context()
    try:
        raw = socket.create_connection((host, port), timeout=CONNECT_TIMEOUT_S)
        tls = ctx.wrap_socket(raw, server_hostname=host)
    except Exception as exc:  # noqa: BLE001
        print(f"connect failed: {type(exc).__name__}: {exc}")
        return 1

    with tls:
        tls.settimeout(CONNECT_TIMEOUT_S)

        tls.sendall(build_connect("pc-mqtt-cmd", cfg["user"], cfg["password"]))
        ptype, body = recv_packet(tls)
        if ptype != PKT_CONNACK or len(body) < 2:
            print(f"unexpected CONNACK: {body.hex()}")
            return 1
        if body[1] != 0:
            print(f"broker refused connection: return code {body[1]}")
            return 1
        print("connected (CONNACK ok)\n")

        tls.sendall(build_subscribe(1, sub_topic, 0))
        ptype, body = recv_packet(tls)
        if ptype == PKT_SUBACK:
            print(f"subscribed to {sub_topic} (granted qos {body[-1]})\n")
        else:
            print(f"subscribe returned packet type 0x{ptype:02x}\n")

        pending = list(commands)
        next_send = time.time() + 0.5
        stop_at = None
        seen = 0

        while True:
            now = time.time()

            if pending and now >= next_send:
                cmd = pending.pop(0)
                tls.sendall(build_publish(cmd_topic, cmd, 1))
                print(f"--> [{len(commands) - len(pending)}/{len(commands)}] {cmd}"
                      f"  (QoS 1, packet_id={len(commands) - len(pending)})")
                next_send = now + args.gap
                if not pending:
                    stop_at = time.time() + args.watch

            if not pending and stop_at is not None and now >= stop_at:
                break

            tls.settimeout(0.2)
            try:
                ptype, body = recv_packet(tls)
            except (socket.timeout, TimeoutError):
                continue
            except ConnectionError as exc:
                print(f"connection ended: {exc}")
                break

            if ptype == PKT_PUBLISH:
                topic, payload = parse_publish(body)
                seen += 1
                print(f"[{time.strftime('%H:%M:%S')}] {topic}\n    {payload}")
            elif ptype in (PKT_PUBACK, PKT_PINGRESP):
                pass   # 自己 QoS 1 发布的确认 / 心跳回应，无需展示
            else:
                print(f"(packet type 0x{ptype:02x}, {len(body)} bytes)")

        print(f"--- {seen} message(s) received ---")

        if not args.no_restore and any(c.startswith("relay:") for c in commands):
            print("--> restoring relays to 000")
            tls.sendall(build_publish(cmd_topic, "relay:000", 1))
            time.sleep(1.2)
            try:
                tls.settimeout(1.0)
                ptype, body = recv_packet(tls)
                if ptype == PKT_PUBLISH:
                    topic, payload = parse_publish(body)
                    print(f"    {topic}\n    {payload}")
            except (socket.timeout, TimeoutError, ConnectionError):
                pass

    return 0


if __name__ == "__main__":
    sys.exit(main())
