#!/usr/bin/env python3
"""MQTT over TLS 连接探针（阳性对照工具）

用途
----
ESP32 连不上 MQTT 时，先在**电脑**上用它连一次同样的 broker + 同样的凭据：

  * 电脑也连不上  -> 问题在 broker / 凭据 / 权限，与固件无关
  * 电脑能连上    -> 问题在固件侧（协议版本、报文构造等）

这一步能把排查范围立刻砍半，避免反复烧录设备。

特点
----
* **只用 Python 标准库**（ssl + socket），不需要 paho-mqtt 等任何依赖
* 手写 MQTT 3.1.1 CONNECT 报文，直接读 CONNACK 返回码
* 凭据从 main/secrets.h 读取，**不打印密码**，只打印长度
* 会检查用户名/密码首尾是否有空格（抄错时的典型症状）

用法
----
    python tools/mqtt_probe.py [secrets.h 路径]

默认路径为仓库内的 main/secrets.h。

    python tools/mqtt_probe.py --ask

交互式手输用户名/密码（密码不回显），用于**验证某个密码是否正确**
而不必先改 secrets.h。典型场景：HiveMQ 控制台的 WebClient 能连上，
但设备连不上——点开 WebClient 的「眼睛」图标看到密码后，
用 --ask 手输它跑一遍，即可确认是"secrets.h 抄错了"还是别的原因。

CONNACK 返回码含义
------------------
    0  ACCEPTED                     连接成功
    1  unacceptable protocol        MQTT 协议版本不被接受
    2  identifier rejected          client_id 被拒
    3  server unavailable           服务端不可用
    4  BAD USERNAME OR PASSWORD     用户名或密码错
    5  NOT AUTHORIZED               认证过了但未授权（凭据不存在/无权限/权限未生效）
"""

import argparse
import base64
import getpass
import hashlib
import os
import re
import socket
import ssl
import struct
import sys

DEFAULT_SECRETS = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "main", "secrets.h"
)

RETURN_CODES = {
    0: "ACCEPTED",
    1: "unacceptable protocol version",
    2: "identifier rejected",
    3: "server unavailable",
    4: "BAD USERNAME OR PASSWORD",
    5: "NOT AUTHORIZED (credential unknown / no permission / permission not active yet)",
}

CONNECT_TIMEOUT_S = 20


def load_secrets(path):
    """从 secrets.h 里取出需要的宏（避免硬编码凭据）。"""
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


def mqtt_str(s):
    b = s.encode()
    return len(b).to_bytes(2, "big") + b


def build_connect(client_id, user, password, keepalive=60):
    """构造 MQTT 3.1.1 CONNECT 报文（报文很小，剩余长度用单字节即可）。"""
    flags = 0x02            # clean session
    if user:
        flags |= 0x80       # username flag
    if password:
        flags |= 0x40       # password flag

    variable_header = (
        mqtt_str("MQTT") + bytes([4, flags]) + keepalive.to_bytes(2, "big")
    )
    payload = mqtt_str(client_id)
    if user:
        payload += mqtt_str(user)
    if password:
        payload += mqtt_str(password)

    body = variable_header + payload
    assert len(body) < 128, "probe packet too large for single-byte remaining length"
    return bytes([0x10, len(body)]) + body


def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="MQTT over TLS 连接探针（阳性对照工具）：只用标准库，不打印密码"
    )
    parser.add_argument(
        "secrets",
        nargs="?",
        default=DEFAULT_SECRETS,
        help="secrets.h 路径（默认 main/secrets.h）",
    )
    parser.add_argument(
        "--ask",
        action="store_true",
        help="交互式手输用户名/密码（密码不回显），用于单独验证某个密码",
    )
    parser.add_argument(
        "--ws-port",
        type=int,
        default=8884,
        help="WebSocket 端口（默认 8884，HiveMQ 控制台 WebClient 走的通道）",
    )
    parser.add_argument(
        "--raw-only",
        action="store_true",
        help="只测裸 MQTT(8883)，不测 WebSocket",
    )
    return parser.parse_args(argv)


def ws_handshake(tls, host, path):
    """MQTT over WebSocket 的 HTTP Upgrade 握手（RFC 6455，子协议 mqtt）。"""
    key = base64.b64encode(os.urandom(16)).decode()
    request = (
        f"GET {path} HTTP/1.1\r\n"
        f"Host: {host}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Protocol: mqtt\r\n"
        "\r\n"
    )
    tls.sendall(request.encode())

    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = tls.recv(4096)
        if not chunk:
            break
        buf += chunk
    return buf.decode(errors="replace")


def ws_send_binary(tls, payload):
    """发一个客户端二进制帧（客户端到服务端必须加掩码）。"""
    mask = os.urandom(4)
    n = len(payload)
    header = bytes([0x82])               # FIN + opcode 0x2 (binary)
    if n < 126:
        header += bytes([0x80 | n])
    elif n < 65536:
        header += bytes([0x80 | 126]) + struct.pack(">H", n)
    else:
        header += bytes([0x80 | 127]) + struct.pack(">Q", n)
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    tls.sendall(header + mask + masked)


def ws_recv_frame(tls):
    """读一个服务端帧并返回其载荷（服务端不加掩码）。"""
    head = tls.recv(2)
    if len(head) < 2:
        return b""
    length = head[1] & 0x7F
    if length == 126:
        length = struct.unpack(">H", tls.recv(2))[0]
    elif length == 127:
        length = struct.unpack(">Q", tls.recv(8))[0]
    data = b""
    while len(data) < length:
        chunk = tls.recv(length - len(data))
        if not chunk:
            break
        data += chunk
    return data


def try_connect(host, port, packet, use_ws, ws_path):
    """返回 (描述文本, CONNACK 字节)；异常向上抛出。"""
    ctx = ssl.create_default_context()
    with socket.create_connection((host, port), timeout=CONNECT_TIMEOUT_S) as raw:
        with ctx.wrap_socket(raw, server_hostname=host) as tls:
            desc = f"TLS {tls.version()}, {tls.cipher()[0]}"
            if not use_ws:
                tls.sendall(packet)
                return desc, tls.recv(4)

            resp = ws_handshake(tls, host, ws_path)
            status = resp.split("\r\n", 1)[0]
            if "101" not in status:
                return f"{desc}; WS upgrade FAILED: {status}", b""
            ws_send_binary(tls, packet)
            return f"{desc}; WS upgrade OK ({ws_path})", ws_recv_frame(tls)


def normalize_uri(uri):
    """容忍 URI 缺少 scheme 的写法。

    esp-mqtt 要求完整 URI（mqtt:// / mqtts:// / ws:// / wss://）；
    手写 secrets.h 时漏掉 scheme 很常见，探针这里做容错以便先诊断出真正原因。
    """
    uri = (uri or "").strip()
    if "//" in uri:
        return uri
    return "mqtts://" + uri


def main():
    args = parse_args(sys.argv[1:])
    secrets_path = args.secrets

    if not os.path.exists(secrets_path):
        print(f"secrets.h not found: {secrets_path}")
        print("copy main/secrets.h.example to main/secrets.h and fill it in first")
        return 2

    cfg = load_secrets(secrets_path)
    if not cfg["uri"]:
        print("SECRET_MQTT_URI is empty in secrets.h")
        return 2

    # --ask：覆盖凭据，用于验证"从 WebClient 看到的密码"是否正确
    if args.ask:
        print("interactive mode: nothing is echoed/printed for the password")
        entered_user = input(f"username [{cfg['user']}]: ").strip() or cfg["user"]
        entered_pw = getpass.getpass("password (not echoed): ")
        cfg["user"] = entered_user
        cfg["password"] = entered_pw

    raw_uri = cfg["uri"]
    cfg["uri"] = normalize_uri(cfg["uri"])
    if raw_uri != cfg["uri"]:
        print(f"note      : URI had no scheme, normalized to {cfg['uri']}")

    host = cfg["uri"].split("//", 1)[1].split(":", 1)[0]
    port = int(cfg["uri"].rsplit(":", 1)[1])

    print(f"secrets   : {secrets_path}" + ("  (overridden by --ask)" if args.ask else ""))
    print(f"broker    : {host}")
    print(f"username  : {cfg['user']!r} (length {len(cfg['user'] or '')})")
    print(f"password  : <hidden> (length {len(cfg['password'] or '')})")

    # 抄凭据时最常见的低级错误：首尾多了空格
    for key in ("user", "password"):
        value = cfg[key] or ""
        if value != value.strip():
            print(f"!! WARNING: {key} has leading/trailing whitespace -> this breaks auth")

    packet = build_connect("pc-probe", cfg["user"], cfg["password"])

    # 两种传输都测：裸 MQTT(8883) 与 WebSocket(8884)。
    # HiveMQ 控制台的 WebClient 走的是 8884/mqtt；若它通而 8883 不通，
    # 就能解释"网页端能连、设备连不上"，并给出一条可用的替代路径
    # （esp-mqtt 同样支持 wss://）。
    attempts = [(port, False, "/mqtt")]
    if not args.raw_only:
        attempts.append((args.ws_port, True, "/mqtt"))

    saw_success = False
    for target_port, use_ws, ws_path in attempts:
        label = (f"wss://{host}:{target_port}{ws_path}" if use_ws
                 else f"mqtts://{host}:{target_port}")
        print(f"\n--- trying {label} ---")
        try:
            desc, resp = try_connect(host, target_port, packet, use_ws, ws_path)
        except Exception as exc:  # noqa: BLE001 - 探针需要如实报出任何异常
            print(f"EXCEPTION: {type(exc).__name__}: {exc}")
            continue

        print(desc)
        if not resp:
            print("no CONNACK received")
            continue
        print(f"CONNACK raw = {resp.hex()}")
        if len(resp) >= 4 and resp[0] == 0x20:
            rc = resp[3]
            print(f"return code = {rc}  ({RETURN_CODES.get(rc, 'unknown')})")
            if rc == 0:
                saw_success = True
        else:
            print("unexpected response (not a CONNACK)")

    return 0 if saw_success else 1


if __name__ == "__main__":
    sys.exit(main())
