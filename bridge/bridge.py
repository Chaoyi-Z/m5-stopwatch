"""
Claude Desktop Assistant — PC Bridge
=====================================
M5Stack StopWatch connects over WiFi WebSocket.
Device records voice via its built-in mic, sends raw PCM binary,
bridge transcribes and returns TRANSCRIPT:.

Protocol:
  Device → PC (text):  CMD:HEARTBEAT:batt=N | CMD:INJECT | CMD:ENTER | CMD:ALLOW | CMD:DENY
  Device → PC (binary): raw 16-bit mono PCM at 16kHz (recorded on device)
  PC → Device (text):  STATUS:idle|thinking|waiting|transcribing
                        TOKENS:in=N,out=N,cost=F
                        USAGE:sess=N,sreset=N,wall=N,wson=N,wreset=str
                        MODEL:friendly-name
                        NOTIFY:message          (general attention)
                        NOTIFYP:message         (permission prompt → Blue = Allow)
                        WEATHER:temp=N,code=N
                        TRANSCRIPT:text
                        CONFIRM:injected
"""
import asyncio
import json
import socket
import time
import urllib.request

import websockets

from stt import transcribe_pcm
from monitor import ClaudeMonitor
from injector import TextInjector


PORT     = 8765
monitor  = ClaudeMonitor()
injector = TextInjector()

_clients: set = set()
_last_transcript: str = ""    # held until CMD:INJECT
_audio_buf: bytearray = bytearray()  # accumulates binary chunks from device mic
# Status reported by a remote (SSH) Claude Code via RSTATUS: (hooks on the cluster)
_remote_status: str = "idle"
_remote_status_ts: float = 0.0

# ── iPhone push via ntfy.sh ─────────────────────────────────────────────────────
# Subscribe to this exact topic in the ntfy iOS app to receive pushes.
try:
    from local_config import NTFY_TOPIC          # gitignored; your private topic
except Exception:
    NTFY_TOPIC = "your-unguessable-ntfy-topic"   # see local_config.example.py
NTFY_URL   = f"https://ntfy.sh/{NTFY_TOPIC}"
# Per-source "done"-edge tracking so pushes can say which environment finished.
_prev_local: str = "idle"           # local Claude Code / Cowork
_prev_remote: str = "idle"          # SSH cluster (via RSTATUS)
_last_done_local_ts: float = 0.0
_last_done_remote_ts: float = 0.0   # separate cooldowns so we never miss a source
_local_stop_ts: float = 0.0         # when local last went non-thinking (0 = working/armed-off)

def _ntfy_sync(title: str, message: str, tags: str = "", priority: str = "default") -> None:
    try:
        req = urllib.request.Request(
            NTFY_URL, data=message.encode("utf-8"), method="POST",
            headers={"Title": title, "Tags": tags, "Priority": priority})
        urllib.request.urlopen(req, timeout=5)
        print(f"[Bridge] ntfy -> {title}: {message}")
    except Exception as e:
        print(f"[Bridge] ntfy failed: {e}")

async def _push(title: str, message: str, tags: str = "", priority: str = "default") -> None:
    # Run the blocking HTTP POST in a thread so the event loop isn't stalled.
    loop = asyncio.get_running_loop()
    await loop.run_in_executor(None, lambda: _ntfy_sync(title, message, tags, priority))

# ── Weather ───────────────────────────────────────────────────────────────────
_WEATHER_LAT      = 42.36    # Cambridge MA (MIT)
_WEATHER_LON      = -71.09
_WEATHER_INTERVAL = 600      # seconds between refreshes

_weather = {"temp": 20, "code": 0, "fetched_at": 0.0}


def _fetch_weather_sync() -> None:
    url = (f"https://api.open-meteo.com/v1/forecast"
           f"?latitude={_WEATHER_LAT}&longitude={_WEATHER_LON}"
           f"&current=temperature_2m,weathercode"
           f"&temperature_unit=celsius&timezone=America%2FNew_York")
    try:
        with urllib.request.urlopen(url, timeout=6) as r:
            data = json.loads(r.read())
        _weather["temp"] = round(data["current"]["temperature_2m"])
        _weather["code"] = int(data["current"]["weathercode"])
        _weather["fetched_at"] = time.monotonic()
        print(f"[Bridge] Weather: {_weather['temp']}°C  code={_weather['code']}")
    except Exception as e:
        print(f"[Bridge] Weather fetch failed: {e}")


# ── Audio handler (binary WebSocket chunks from device mic) ───────────────────
# Device streams 4KB chunks while sending; bridge accumulates until RECORD_STOP.

async def _handle_audio(ws, data: bytes):
    _audio_buf.extend(data)
    # Show RMS level every ~0.5s (16 chunks of 512 bytes = 8192 bytes = 0.256s)
    if len(_audio_buf) % 8192 < 512:
        import struct, math
        samples = struct.unpack(f"<{len(data)//2}h", data)
        rms = math.sqrt(sum(s*s for s in samples) / len(samples)) if samples else 0
        print(f"[Bridge] Audio: {len(_audio_buf):,} bytes  RMS={rms:.0f}")


# ── Text message handler ──────────────────────────────────────────────────────

async def _handle_message(ws, msg: str):
    global _last_transcript, _audio_buf, _remote_status, _remote_status_ts

    # ── Messages from a remote (SSH) Claude Code, via hooks on the cluster ──────
    if msg.startswith("RSTATUS:"):
        _remote_status = msg[len("RSTATUS:"):].strip()
        _remote_status_ts = time.monotonic()
        print(f"[Bridge] Remote status: {_remote_status}")
        return

    if msg.startswith("RNOTIFYP:") or msg.startswith("RNOTIFY:"):
        is_perm = msg.startswith("RNOTIFYP:")
        body = msg.split(":", 1)[1].strip() or "Claude needs you (remote)"
        cmd = "NOTIFYP" if is_perm else "NOTIFY"
        dead = set()
        for c in list(_clients):
            try:
                await c.send(f"{cmd}:{body}")
            except Exception:
                dead.add(c)
        _clients -= dead
        print(f"[Bridge] Remote {cmd} -> device: {body!r}")
        if is_perm:
            await _push("Claude — permission", f"{body} - Cluster (SSH)",
                        tags="warning", priority="high")
        return

    if msg.startswith("CMD:RECORD_START"):
        _audio_buf = bytearray()
        print("[Bridge] Recording started — buffer cleared.")

    elif msg.startswith("CMD:RECORD_STOP"):
        data = bytes(_audio_buf)
        _audio_buf = bytearray()
        secs = len(data) / 2 / 16000
        print(f"[Bridge] Recording done: {secs:.1f}s  ({len(data):,} bytes)")
        if secs < 0.3:
            print("[Bridge] Audio too short — ignoring.")
            await ws.send("TRANSCRIPT:")
            return
        loop = asyncio.get_running_loop()
        transcript = await loop.run_in_executor(None, lambda: transcribe_pcm(data))
        _last_transcript = transcript
        await ws.send(f"TRANSCRIPT:{transcript}")

    elif msg.startswith("CMD:INJECT"):
        if _last_transcript:
            print(f"[Bridge] Injecting: {_last_transcript!r}")
            injector.inject(_last_transcript)
            _last_transcript = ""
            await ws.send("CONFIRM:injected")
        else:
            print("[Bridge] CMD:INJECT with no transcript.")

    elif msg.startswith("CMD:ENTER"):
        print("[Bridge] Pressing Enter")
        injector.submit()
        await ws.send("CONFIRM:enter")

    elif msg.startswith("CMD:ALLOW"):
        print("[Bridge] Approving permission prompt (Ctrl+Enter)")
        injector.allow()
        await ws.send("CONFIRM:allow")

    elif msg.startswith("CMD:DENY"):
        print("[Bridge] Denying permission prompt (Escape)")
        injector.deny()
        await ws.send("CONFIRM:deny")

    elif msg.startswith("CMD:HEARTBEAT"):
        if ":batt=" in msg:
            try:
                batt = int(msg.split(":batt=")[1])
                print(f"[Bridge] Heartbeat — battery: {batt}%")
            except ValueError:
                pass


# ── WebSocket connection handler ──────────────────────────────────────────────

async def _handle_client(ws):
    _clients.add(ws)
    print(f"[Bridge] Device connected from {ws.remote_address}")
    try:
        async for message in ws:
            if isinstance(message, bytes):
                await _handle_audio(ws, message)
            else:
                await _handle_message(ws, str(message))
    except Exception:
        pass
    finally:
        _clients.discard(ws)
        print(f"[Bridge] Device disconnected: {ws.remote_address}")


# ── Background status broadcaster ────────────────────────────────────────────

async def _status_loop():
    global _clients, _prev_local, _prev_remote, _last_done_local_ts, _last_done_remote_ts, _local_stop_ts
    loop = asyncio.get_running_loop()

    # Fetch weather once at startup
    await loop.run_in_executor(None, _fetch_weather_sync)

    while True:
        await asyncio.sleep(1)   # 1s poll → snappier thinking/done detection
        if not _clients:
            continue

        # Refresh weather every 10 minutes
        if time.monotonic() - _weather["fetched_at"] > _WEATHER_INTERVAL:
            await loop.run_in_executor(None, _fetch_weather_sync)

        local_status  = monitor.get_status()
        remote_fresh  = time.monotonic() - _remote_status_ts < 600
        remote_status = _remote_status if remote_fresh else "idle"
        now_m = time.monotonic()

        # iPhone "done" push per source (thinking → not-thinking), labeled so you know
        # which environment finished. Separate 20s cooldowns guard against flap spam.
        # Debounced: only push once local has STAYED non-thinking ~12s. Concurrent
        # sessions/subagents make the "latest file" flap thinking↔waiting for a few
        # seconds; a real completion stays done, a flap flips back — so we wait it out.
        if local_status == "thinking":
            _local_stop_ts = 0.0
        else:
            if _prev_local == "thinking":
                _local_stop_ts = now_m                 # just stopped — start settle timer
            elif _local_stop_ts and now_m - _local_stop_ts >= 12 \
                    and now_m - _last_done_local_ts > 20:
                _last_done_local_ts = now_m
                _local_stop_ts = 0.0                    # disarm; fire once
                await _push("Claude", f"Job done - {monitor.get_source_label()}",
                            tags="white_check_mark")
        _prev_local = local_status
        if _prev_remote == "thinking" and remote_status != "thinking" \
                and now_m - _last_done_remote_ts > 20:
            _last_done_remote_ts = now_m
            await _push("Claude", "Job done - Cluster (SSH)", tags="white_check_mark")
        _prev_remote = remote_status

        # Merged status for the watch: 'thinking' wins so the spinner shows;
        # 'waiting' shows (and chimes) when local is idle. Remote stale after 10 min.
        status = local_status
        if remote_status == "thinking":
            status = "thinking"
        elif status == "idle" and remote_status == "waiting":
            status = "waiting"

        tokens = monitor.get_tokens()
        daily  = monitor.get_daily()
        usage  = monitor.get_usage_limits()
        model  = monitor.get_model()
        msgs = [
            f"STATUS:{status}",
            f"TOKENS:in={tokens['in']},out={tokens['out']},cost={tokens['cost']:.4f}"
            f",din={daily['din']},dout={daily['dout']},dcost={daily['dcost']:.4f}",
            f"WEATHER:temp={_weather['temp']},code={_weather['code']}",
        ]
        if model:
            msgs.append(f"MODEL:{model}")

        # One-shot attention alert. NOTIFYP = permission prompt (watch Blue = Allow);
        # NOTIFY = general attention.
        notify = monitor.pop_notification()
        if notify and notify["msg"]:
            cmd = "NOTIFYP" if notify["kind"] == "permission" else "NOTIFY"
            print(f"[Bridge] {cmd} -> device: {notify['msg']!r}")
            msgs.append(f"{cmd}:{notify['msg']}")
            if notify["kind"] == "permission":
                await _push("Claude — permission",
                            f"{notify['msg']} - {monitor.get_source_label()}",
                            tags="warning", priority="high")
        if usage:
            msgs.append(
                f"USAGE:sess={usage.get('session_pct',0)}"
                f",sreset={usage.get('session_reset_min',0)}"
                f",wall={usage.get('weekly_all_pct',0)}"
                f",wson={usage.get('weekly_sonnet_pct',0)}"
                f",wreset={usage.get('weekly_reset_str','')}"
            )

        dead = set()
        for ws in list(_clients):
            try:
                for m in msgs:
                    await ws.send(m)
            except Exception:
                dead.add(ws)
        _clients -= dead


# ── Main ──────────────────────────────────────────────────────────────────────

async def _main():
    try:
        local_ip = socket.gethostbyname(socket.gethostname())
    except Exception:
        local_ip = "127.0.0.1"

    print("=" * 52)
    print("  Claude Desktop Assistant — Bridge")
    print(f"  ws://{local_ip}:{PORT}")
    print(f"  Set BRIDGE_IP to \"{local_ip}\" in firmware/src/config.h")
    print("=" * 52)

    async with websockets.serve(_handle_client, "0.0.0.0", PORT):
        print("[Bridge] Waiting for device…")
        await _status_loop()


if __name__ == "__main__":
    asyncio.run(_main())
