"""
Monitors Claude Code processes and reads real token data from JSONL session files.

Token data is read from:
  C:\\Users\\<you>\\.claude\\projects\\<project-slug>\\<sessionId>.jsonl

Each assistant message contains:
  {"type":"assistant","message":{"usage":{
      "input_tokens": N,
      "cache_creation_input_tokens": N,
      "cache_read_input_tokens": N,
      "output_tokens": N
  }}}
"""
import datetime
import json
import re
import time
import urllib.request
import psutil
from pathlib import Path


CLAUDE_NAMES = {"claude", "claude.exe"}
# VS Code is intentionally excluded: its background CPU (IntelliSense, extensions,
# language servers) constantly exceeds the 15% threshold and causes false "thinking"
# triggers. Only the Claude Code CLI process itself is a reliable signal.

# Claude Sonnet 4 pricing (per token)
_COST_IN       = 3.00  / 1_000_000
_COST_OUT      = 15.00 / 1_000_000
_COST_CACHE_W  =  3.75 / 1_000_000   # cache write (creation)
_COST_CACHE_R  =  0.30 / 1_000_000   # cache read

_DAILY_TTL = 60.0   # re-scan all today's files every 60 seconds


class ClaudeMonitor:
    def __init__(self):
        self._session_start = time.time()
        # Current-session incremental reader
        self._jsonl_path: Path | None = None
        self._jsonl_pos: int = 0
        self._tok_in    = 0
        self._tok_out   = 0
        self._tok_cw    = 0
        self._tok_cr    = 0
        # Daily totals cache
        self._daily: dict = {"din": 0, "dout": 0, "dcost": 0.0}
        self._daily_ts: float = 0.0
        # Usage limits cache (from claude.ai OAuth)
        self._usage: dict = {}
        self._usage_ts: float = 0.0
        # Permission/attention notifications (written by notify_hook.py).
        # Start at 0 and rely on the freshness window in pop_notification: a prompt
        # that fired during a restart/reboot still buzzes on reconnect, while stale
        # notifications from a past session don't replay.
        self._notify_file = Path(__file__).resolve().parent / "notify.json"
        self._last_notify_ts: float = 0.0

    # ── Status ────────────────────────────────────────────────────────────────

    def get_status(self) -> str:
        """Return 'thinking' | 'waiting' | 'idle'.

        Uses JSONL file content rather than CPU% — Claude's API calls are
        I/O-bound so CPU is near 0 even during active generation.

        Logic:
          Last entry type == 'user'              → Claude hasn't replied yet    → thinking
          Last entry type == 'tool'              → tool result pending response  → thinking
          Last entry == 'assistant' + tool_use   → mid-agent loop               → thinking
          Last entry == 'assistant' + text       → final response written       → waiting
        """
        if not self._find_claude_procs():
            return "idle"

        latest = self._find_latest_jsonl()
        if latest is None:
            return "waiting"

        try:
            mtime_age = time.time() - latest.stat().st_mtime
        except OSError:
            return "waiting"

        if mtime_age > 600:          # 10 min untouched = stale session
            return "idle"

        # Cowork (desktop local-agent-mode): its audit log only ends on a "result"
        # entry when the WHOLE task finishes; between steps the last entry varies, so
        # plain last-entry parsing flaps thinking↔done and spams "Job done". Treat
        # Cowork as still working until a terminal "result" entry appears.
        if "local-agent-mode-sessions" in str(latest):
            return "waiting" if self._cowork_finished(latest) else "thinking"

        state = self._last_entry_state(latest)
        if state == "thinking":
            return "thinking"
        elif state == "done":
            return "waiting"
        else:
            # Tail unparseable — fall back to recency
            return "thinking" if mtime_age < 30 else "waiting"

    def _last_entry_state(self, path: Path) -> str:
        """Read the last substantive JSONL entry; return 'thinking' | 'done' | 'unknown'."""
        try:
            size = path.stat().st_size
            # Read a generous tail: the final assistant message can be tens of KB.
            # Too small a window lands mid-line, fails to parse, and forces the
            # recency fallback — which keeps reporting "thinking" for ~30s after
            # Claude is actually done, delaying the done-chime.
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                f.seek(max(0, size - 65536))
                tail = f.read()

            for line in reversed(tail.splitlines()):
                line = line.strip()
                if not line:
                    continue
                try:
                    obj = json.loads(line)
                except json.JSONDecodeError:
                    continue

                t = obj.get("type", "")
                if t == "assistant":
                    msg = obj.get("message", {})
                    content = msg.get("content", [])
                    if isinstance(content, list) and any(
                        c.get("type") == "tool_use" for c in content
                    ):
                        return "thinking"   # tool call in flight, more steps pending
                    return "done"           # text end_turn — all work complete
                if t in ("user", "tool"):
                    return "thinking"       # user query or tool result awaiting response
                # skip: summary, system, thinking-block, etc.

        except OSError:
            pass
        return "unknown"

    def _cowork_finished(self, path: Path) -> bool:
        """True only if the Cowork audit log's last substantive entry is a terminal
        'result' — i.e. the whole task finished. Any assistant/user/tool/system entry
        means it's still mid-task (so we report 'thinking' and don't fire a done push)."""
        try:
            size = path.stat().st_size
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                f.seek(max(0, size - 65536))
                tail = f.read()
            for line in reversed(tail.splitlines()):
                line = line.strip()
                if not line:
                    continue
                try:
                    t = json.loads(line).get("type", "")
                except json.JSONDecodeError:
                    continue
                if t == "result":
                    return True
                if t in ("assistant", "user", "tool", "system"):
                    return False
        except OSError:
            pass
        return False

    # ── Permission / attention notifications ─────────────────────────────────────

    def pop_notification(self):
        """Return a freshly-arrived notification dict {"msg","kind"} once, else None.

        notify.json is written by notify_hook.py (the Claude Code Notification /
        PermissionRequest hooks) whenever Claude needs the user. Each timestamp
        fires at most once. We also require it to be recent (<30s) so a prompt that
        fired during a bridge restart / device reboot still buzzes on reconnect,
        but a leftover from a past session doesn't replay on startup.
        kind is "permission" (awaiting approval → watch Blue = Allow) or "notification".
        """
        try:
            data = json.loads(self._notify_file.read_text(encoding="utf-8"))
            ts = float(data.get("ts", 0.0))
        except (OSError, ValueError, json.JSONDecodeError):
            return None
        if ts <= self._last_notify_ts:
            return None
        self._last_notify_ts = ts                # advance so we don't re-check it
        if time.time() - ts < 30.0:
            return {"msg": str(data.get("msg", "")).strip(),
                    "kind": data.get("kind", "notification")}
        return None

    # ── Current model ───────────────────────────────────────────────────────────

    def get_model(self) -> str:
        """Friendly name of the model used by the most recent assistant message.

        Reads the tail of the latest session JSONL and returns e.g. "Opus 4.8".
        Returns "" if no model can be determined.
        """
        latest = self._find_latest_jsonl()
        if latest is None:
            return ""
        try:
            size = latest.stat().st_size
            with open(latest, "r", encoding="utf-8", errors="replace") as f:
                f.seek(max(0, size - 8192))
                tail = f.read()
            for line in reversed(tail.splitlines()):
                line = line.strip()
                if not line:
                    continue
                try:
                    obj = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if obj.get("type") == "assistant":
                    mid = obj.get("message", {}).get("model", "")
                    if mid:
                        return self._pretty_model(mid)
        except OSError:
            pass
        return ""

    @staticmethod
    def _pretty_model(model_id: str) -> str:
        """'claude-sonnet-4-6-20250929' → 'Sonnet 4.6'. Falls back to the raw id."""
        mid = model_id.lower()
        family = ""
        for fam in ("opus", "sonnet", "haiku", "fable"):
            if fam in mid:
                family = fam.capitalize()
                break
        if not family:
            return model_id
        # First two numeric groups after the family form major.minor.
        nums = re.findall(r"\d+", mid.split(family.lower(), 1)[1])
        if len(nums) >= 2:
            return f"{family} {nums[0]}.{nums[1]}"
        if len(nums) == 1:
            return f"{family} {nums[0]}"
        return family

    # ── Source label ─────────────────────────────────────────────────────────────

    def get_source_label(self) -> str:
        """Human label for whichever session is currently latest — so pushes can say
        which environment finished. e.g. 'Local: Stopwatch' or 'Cowork (desktop)'."""
        latest = self._find_latest_jsonl()
        if latest is None:
            return "Claude Code"
        if "local-agent-mode-sessions" in str(latest):
            return "Cowork (desktop)"
        proj = self._session_project(latest)
        return f"Local: {proj}" if proj else "Local (Claude Code)"

    def _session_project(self, path: Path) -> str:
        """Basename of the session's cwd (its project folder), best-effort."""
        try:
            size = path.stat().st_size
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                f.seek(max(0, size - 65536))
                tail = f.read()
            m = re.findall(r'"cwd"\s*:\s*"([^"]+)"', tail)
            if m:
                parts = [p for p in re.split(r'[\\/]+', m[-1]) if p]
                if parts:
                    return parts[-1]
        except OSError:
            pass
        slug = path.parent.name           # fallback: last token of the project slug
        return slug.split("-")[-1] if slug else ""

    # ── Session tokens ────────────────────────────────────────────────────────

    def get_tokens(self) -> dict:
        """Current-session token counts from the active JSONL file."""
        self._ingest_new_lines()
        display_in = self._tok_in + self._tok_cw + self._tok_cr
        cost = (self._tok_in  * _COST_IN  +
                self._tok_out * _COST_OUT +
                self._tok_cw  * _COST_CACHE_W +
                self._tok_cr  * _COST_CACHE_R)
        return {"in": display_in, "out": self._tok_out, "cost": cost}

    # ── Daily totals ──────────────────────────────────────────────────────────

    def get_daily(self) -> dict:
        """
        Total tokens across ALL Claude Code sessions today.
        Cached for 60 seconds to avoid re-scanning on every 2-second status tick.
        Returns {"din": N, "dout": N, "dcost": F}
        """
        if time.monotonic() - self._daily_ts < _DAILY_TTL:
            return self._daily

        projects = Path.home() / ".claude" / "projects"
        today = datetime.date.today()
        t_in = t_out = t_cw = t_cr = 0

        if projects.exists():
            for f in projects.glob("**/*.jsonl"):
                try:
                    if datetime.date.fromtimestamp(f.stat().st_mtime) != today:
                        continue
                    with open(f, "r", encoding="utf-8") as fh:
                        for raw in fh:
                            raw = raw.strip()
                            if not raw:
                                continue
                            try:
                                obj = json.loads(raw)
                                if obj.get("type") == "assistant":
                                    u = obj.get("message", {}).get("usage", {})
                                    t_in  += u.get("input_tokens", 0)
                                    t_out += u.get("output_tokens", 0)
                                    t_cw  += u.get("cache_creation_input_tokens", 0)
                                    t_cr  += u.get("cache_read_input_tokens", 0)
                            except (json.JSONDecodeError, KeyError):
                                pass
                except OSError:
                    pass

        dcost = (t_in * _COST_IN + t_out * _COST_OUT +
                 t_cw * _COST_CACHE_W + t_cr * _COST_CACHE_R)
        self._daily = {"din": t_in + t_cw + t_cr, "dout": t_out, "dcost": dcost}
        self._daily_ts = time.monotonic()
        print(f"[Monitor] Daily total: {self._daily['din']+self._daily['dout']:,} tokens  ${dcost:.4f}")
        return self._daily

    # ── Anthropic account usage limits ───────────────────────────────────────

    def get_usage_limits(self) -> dict:
        """
        Fetch rate-limit usage via the Claude Code OAuth token.
        Returns dict with keys: session_pct, session_reset_min,
                                weekly_all_pct, weekly_sonnet_pct, weekly_reset_str
        Returns the cached value on failure.
        Cached 60 s on success, 300 s on failure.
        """
        if time.monotonic() - self._usage_ts < 60.0:
            return self._usage

        creds_file = Path.home() / ".claude" / ".credentials.json"
        try:
            with open(creds_file) as f:
                creds = json.load(f)
            token = creds["claudeAiOauth"]["accessToken"]
        except (OSError, KeyError, json.JSONDecodeError) as e:
            print(f"[Monitor] Credentials unavailable: {e}")
            return self._usage

        # OAuth token (sk-ant-oat...) authenticates against api.anthropic.com.
        # Requires the oauth beta header; the claude.ai web API (cookie-based)
        # rejects this token with 403.
        headers = {
            "Authorization":   f"Bearer {token}",
            "anthropic-beta":  "oauth-2025-04-20",
            "anthropic-version": "2023-06-01",
            "Content-Type":    "application/json",
        }
        url = "https://api.anthropic.com/api/oauth/usage"

        try:
            req = urllib.request.Request(url, headers=headers)
            with urllib.request.urlopen(req, timeout=8) as r:
                data = json.loads(r.read())
            parsed = self._parse_usage(data)
            if parsed:
                self._usage    = parsed
                self._usage_ts = time.monotonic()
                print(f"[Monitor] Usage: session {parsed['session_pct']}%  "
                      f"weekly {parsed['weekly_all_pct']}%  "
                      f"sonnet {parsed['weekly_sonnet_pct']}%")
                return self._usage
        except Exception as e:
            print(f"[Monitor] Usage fetch failed: {e}")

        # Failed — back off 5 minutes before trying again
        self._usage_ts = time.monotonic() - 60.0 + 300.0
        return self._usage

    def _parse_usage(self, data: dict) -> dict:
        """
        Parse the /api/oauth/usage response. Shape:
          {"five_hour":        {"utilization": 3.0, "resets_at": "<iso>"},
           "seven_day":        {"utilization": 8.0, "resets_at": "<iso>"},
           "seven_day_sonnet": {"utilization": 9.0, "resets_at": "<iso>"}, ...}
        """
        five = data.get("five_hour") or {}
        week = data.get("seven_day") or {}
        son  = data.get("seven_day_sonnet") or {}
        if not (five or week):
            print(f"[Monitor] Unknown usage shape — keys: {list(data.keys())}")
            return {}

        return {
            "session_pct":       int(round(five.get("utilization", 0))),
            "session_reset_min": self._mins_until(five.get("resets_at", "")),
            "weekly_all_pct":    int(round(week.get("utilization", 0))),
            "weekly_sonnet_pct": int(round(son.get("utilization", 0))),
            "weekly_reset_str":  self._fmt_reset(week.get("resets_at", "")),
        }

    @staticmethod
    def _mins_until(iso: str) -> int:
        """Minutes from now until an ISO8601 timestamp (0 if past/unparseable)."""
        if not iso:
            return 0
        try:
            dt = datetime.datetime.fromisoformat(iso)
            now = datetime.datetime.now(datetime.timezone.utc)
            return max(0, int((dt - now).total_seconds() // 60))
        except ValueError:
            return 0

    @staticmethod
    def _fmt_reset(iso: str) -> str:
        """ISO8601 (UTC) → compact local string like 'Sat 6:00PM' ('' if unparseable)."""
        if not iso:
            return ""
        try:
            dt = datetime.datetime.fromisoformat(iso).astimezone()
            hour12 = dt.hour % 12 or 12
            ampm = "AM" if dt.hour < 12 else "PM"
            return f"{dt.strftime('%a')} {hour12}:{dt.minute:02d}{ampm}"
        except ValueError:
            return ""

    # ── Internal helpers ──────────────────────────────────────────────────────

    def _find_claude_procs(self) -> list:
        procs = []
        for p in psutil.process_iter(["name"]):
            try:
                name = (p.info["name"] or "").lower()
                if name in CLAUDE_NAMES:
                    procs.append(p)
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                pass
        return procs

    def _find_latest_jsonl(self) -> Path | None:
        # Scan both Claude Code sessions AND the Claude desktop app's Cowork
        # ("local-agent-mode") sessions, so the watch tracks whichever is active.
        roots = [
            Path.home() / ".claude" / "projects",
            Path.home() / "AppData" / "Roaming" / "Claude" / "local-agent-mode-sessions",
        ]
        files = []
        for r in roots:
            if r.exists():
                files.extend(r.glob("**/*.jsonl"))
        # Pick the most recently modified, tolerating files that vanish between the
        # glob and the stat (Cowork rotates/deletes its session logs concurrently).
        best, best_mt = None, -1.0
        for f in files:
            try:
                mt = f.stat().st_mtime
            except OSError:
                continue
            if mt > best_mt:
                best, best_mt = f, mt
        return best

    def _ingest_new_lines(self):
        latest = self._find_latest_jsonl()
        if latest is None:
            return

        if latest != self._jsonl_path:
            print(f"[Monitor] Reading session: {latest.name}")
            self._jsonl_path = latest
            self._jsonl_pos  = 0
            self._tok_in  = 0
            self._tok_out = 0
            self._tok_cw  = 0
            self._tok_cr  = 0

        try:
            with open(latest, "r", encoding="utf-8") as f:
                f.seek(self._jsonl_pos)
                for raw in f:
                    line = raw.strip()
                    if not line:
                        continue
                    try:
                        obj = json.loads(line)
                        if obj.get("type") == "assistant":
                            u = obj.get("message", {}).get("usage", {})
                            self._tok_in  += u.get("input_tokens", 0)
                            self._tok_out += u.get("output_tokens", 0)
                            self._tok_cw  += u.get("cache_creation_input_tokens", 0)
                            self._tok_cr  += u.get("cache_read_input_tokens", 0)
                    except (json.JSONDecodeError, KeyError):
                        pass
                self._jsonl_pos = f.tell()
        except OSError as e:
            print(f"[Monitor] Cannot read JSONL: {e}")
