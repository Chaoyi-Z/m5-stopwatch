# M5Stack StopWatch — Claude Code companion

Turns an [M5Stack StopWatch](https://docs.m5stack.com/en/core/StopWatch) (ESP32-S3,
1.75" round AMOLED, touch, mic, speaker) into a desktop companion for **Claude Code**:
a glanceable status spinner, a done-chime, live usage limits, voice dictation, and
remote **Allow/Deny** of permission prompts — with matching **iPhone push** notifications.

It tracks Claude activity from **local Claude Code**, **SSH/cluster sessions**, and the
**Claude desktop app's Cowork mode**.

## How it works

```
  Watch  ──WiFi WebSocket──►  Bridge (PC, Python)  ──reads──►  Claude Code session files
 (ESP32-S3)                    ws://<PC>:8765                    (+ hooks for permissions)
```

- **`firmware/`** — PlatformIO ESP32-S3 project (M5Unified, WebSockets, ArduinoJson).
- **`bridge/`** — Python WebSocket server on the PC. Watches Claude Code / Cowork session
  logs for status, reads Anthropic usage limits, does speech-to-text, injects transcripts,
  and pushes to your phone via [ntfy.sh](https://ntfy.sh).

## Features

- **Status spinner** — the watch wakes and spins while Claude is working, per session source.
- **Done-chime + vibration** when Claude finishes; **quiet ~20% volume**.
- **Usage screen** — session / weekly (All + Sonnet) rate-limit bars + current model.
- **Permission prompts** on the watch — **Blue = Allow**, **Yellow = Deny** (sends the
  keystroke to the focused window), with a banner and alert.
- **Voice dictation** — hold Yellow to record → on-device mic → STT on the PC → Blue pastes,
  Blue again presses Enter.
- **iPhone push** (ntfy) on job-done and permission prompts, labeled by source
  (`Local: <project>`, `Cowork`, `Cluster (SSH)`).
- **Battery-minded** — CPU downclock when the screen is off, dimmed AMOLED, WiFi auto-reconnect.

## Setup

### Firmware
1. Install [PlatformIO](https://platformio.org/).
2. `cp firmware/src/config.h.example firmware/src/config.h` and fill in your WiFi + PC IP.
3. Flash: `pio run -e m5stack-stopwatch -t upload` (from `firmware/`).

### Bridge (PC)
1. `pip install -r bridge/requirements.txt`
2. (Optional, for iPhone push) `cp bridge/local_config.example.py bridge/local_config.py`
   and set an unguessable `NTFY_TOPIC`; subscribe to it in the ntfy app.
3. Run: `python bridge/bridge.py` — it prints the IP to put in `config.h`.

### Permission alerts for Claude Code
The bridge relies on Claude Code hooks (`PermissionRequest`, `Notification`) in
`~/.claude/settings.json` calling `bridge/notify_hook.py`. For SSH/cluster sessions, deploy
`bridge/notify_remote.py` to the remote `~/.claude/` and add hooks there (set `BRIDGE_HOST`
to your PC's IP).

## Notes
- `config.h` and `bridge/local_config.py` are gitignored — they hold your WiFi password and
  ntfy topic. Use the `.example` files as templates.
- Built for and tested on Windows; paths in the `.bat`/`.vbs` launchers are examples.
