"""Remote (SSH/cluster) → Stopwatch bridge notifier.

Lives on the remote host (e.g. eofe-login) at ~/.claude/notify_remote.py and is
invoked by Claude Code hooks there. It opens a WebSocket to the bridge running on
your PC and sends a one-line status/notification, so the watch shows the spinner /
done-chime / permission alerts for SSH Claude sessions too.

Stdlib only (no `websockets` package needed). Never raises — must not break Claude.

Hook usage (in the REMOTE ~/.claude/settings.json):
    UserPromptSubmit / PreToolUse / PostToolUse  ->  notify_remote.py RSTATUS:thinking
    Stop                                         ->  notify_remote.py RSTATUS:waiting
    PermissionRequest                            ->  notify_remote.py RNOTIFYP   (tool from stdin)
"""
import base64
import json
import os
import socket
import struct
import sys
import time

# ── Bridge address (your PC's IP on the MIT network) ─────────────────────────
BRIDGE_HOST = "YOUR_PC_LAN_IP"   # the PC running bridge.py, reachable from the remote host
BRIDGE_PORT = 8765


def ws_send(text: str, timeout: float = 5.0) -> None:
    key = base64.b64encode(os.urandom(16)).decode()
    req = (
        "GET / HTTP/1.1\r\n"
        f"Host: {BRIDGE_HOST}:{BRIDGE_PORT}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n"
    )
    s = socket.create_connection((BRIDGE_HOST, BRIDGE_PORT), timeout)
    try:
        s.sendall(req.encode())
        # Drain the handshake response
        resp = b""
        while b"\r\n\r\n" not in resp:
            chunk = s.recv(1024)
            if not chunk:
                break
            resp += chunk
        # One masked text frame (client frames MUST be masked)
        payload = text.encode()
        mask = os.urandom(4)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        ln = len(payload)
        header = b"\x81"
        if ln < 126:
            header += bytes([0x80 | ln])
        elif ln < 65536:
            header += bytes([0x80 | 126]) + struct.pack("!H", ln)
        else:
            header += bytes([0x80 | 127]) + struct.pack("!Q", ln)
        s.sendall(header + mask + masked)
        time.sleep(0.25)  # give the server a moment to read before close
    finally:
        try:
            s.close()
        except OSError:
            pass


def main() -> None:
    arg = sys.argv[1] if len(sys.argv) > 1 else "RSTATUS:thinking"
    if arg == "RNOTIFYP":
        try:
            data = json.load(sys.stdin)
        except Exception:
            data = {}
        tool = data.get("tool_name") or ""
        arg = f"RNOTIFYP:Permission: {tool}" if tool else "RNOTIFYP:Claude needs permission"
    try:
        ws_send(arg)
    except Exception:
        pass  # never break Claude Code


if __name__ == "__main__":
    main()
