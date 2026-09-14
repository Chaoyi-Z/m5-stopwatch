"""Claude Code `Notification` hook → flag file the bridge forwards to the watch.

Registered as a Notification hook in ~/.claude/settings.json. Claude Code pipes a
JSON event on stdin (with a 'message' field) whenever it needs the user's
attention — most importantly a tool-permission prompt. We stamp the message to
notify.json; the bridge polls that file and broadcasts NOTIFY: to the device,
which responds with a distinct sound + vibration.

The hook must never block or error out Claude Code, so all failures are swallowed.
"""
import json
import sys
import time
from pathlib import Path

NOTIFY_FILE = Path(__file__).resolve().parent / "notify.json"


def main() -> None:
    try:
        data = json.load(sys.stdin)
    except Exception:
        data = {}
    # Notification hooks carry a 'message'; PermissionRequest hooks carry 'tool_name'.
    # 'kind' lets the watch know a permission prompt is awaiting approval (Blue = Allow).
    raw = data.get("message")
    if raw:
        msg = str(raw).strip()
        kind = "permission" if "permission" in msg.lower() else "notification"
    else:
        tool = data.get("tool_name") or ""
        msg = f"Permission: {tool}" if tool else "Claude needs your permission"
        kind = "permission"
    try:
        NOTIFY_FILE.write_text(
            json.dumps({"ts": time.time(), "msg": msg, "kind": kind}), encoding="utf-8"
        )
    except OSError:
        pass


if __name__ == "__main__":
    main()
