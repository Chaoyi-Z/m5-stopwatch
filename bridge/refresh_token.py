"""
Keep the Claude Code OAuth access token fresh for the bridge/widget.

The `claude` CLI refreshes the short-lived access token (in ~/.claude/.credentials.json)
from the stored refresh token, but only when it's actually expired. So this checker
runs frequently (every ~15 min via the ClaudeTokenRefresh task) and invokes a tiny
`claude -p` call ONLY when the token is expired or within a minute of it. That keeps
`claude` calls to ~3/day (one per ~8h token cycle) while ensuring the token is never
stale for more than the check interval — well inside the widget's 30-min display
window, so Claude usage never goes blank.

This uses the official CLI's own refresh; it does not read the refresh token or call
OAuth endpoints directly.
"""
import json
import subprocess
import time
from pathlib import Path

CLAUDE_CMD = r"C:\Users\Owner\AppData\Roaming\npm\claude.cmd"
CREDS = Path.home() / ".claude" / ".credentials.json"


def main():
    try:
        exp = json.load(open(CREDS))["claudeAiOauth"]["expiresAt"] / 1000.0
    except Exception:
        exp = 0.0
    if exp - time.time() < 60:          # expired or about to expire
        try:
            subprocess.run([CLAUDE_CMD, "-p", "ok"],
                           capture_output=True, timeout=120)
        except Exception:
            pass


if __name__ == "__main__":
    main()
