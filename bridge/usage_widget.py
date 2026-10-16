"""
AI Usage — a small always-on-top Windows desktop widget that shows Claude Code
and ChatGPT/Codex usage limits (and live working status), mirroring the watch's
page 2. Reuses the bridge's ClaudeMonitor so it needs no extra data plumbing and
works even when the bridge isn't running.

Run:  pythonw usage_widget.py      (no console)
  or:  python  usage_widget.py      (with console / logs)
Drag the header to move it; click the x to close. Position is remembered.
"""
import os
import sys
import time
import threading
import tkinter as tk
from pathlib import Path

try:
    import pystray
    from PIL import Image, ImageDraw
    _HAS_TRAY = True
except Exception:
    _HAS_TRAY = False

# Import the bridge's monitor (same folder).
sys.path.insert(0, str(Path(__file__).resolve().parent))
from monitor import ClaudeMonitor  # noqa: E402

# ── Palette (matches the firmware usage screen) ───────────────────────────────
BG        = "#0E0E14"
CARD      = "#15151F"
BAR_BG    = "#1E1E2E"
TXT       = "#E6E6F0"
DIM       = "#8A8AA0"
FAINT     = "#5A5A72"
NEAR_LIMIT = "#FF4646"          # bar flips red at >=90%

CLAUDE_NAME  = "#00B4FF"
CLAUDE_5H    = "#0096FF"
CLAUDE_WEEK  = "#AF78FF"
CLAUDE_FABLE = "#2FD4C4"          # model-scoped weekly (e.g. Fable)
GPT_NAME    = "#FF5A46"
GPT_5H      = "#FF5A46"
GPT_WEEK    = "#FFA528"

DOT_IDLE    = "#3CC85A"
DOT_THINK   = "#FFC83C"
DOT_WAIT    = "#3CC85A"
DOT_OFF     = "#555566"

W, H = 340, 276
POS_FILE = Path(__file__).resolve().parent / ".usage_widget_pos"

def _tray_image():
    """A tiny 3-bar chart icon for the system tray."""
    img = Image.new("RGBA", (64, 64), (14, 14, 20, 255))
    d = ImageDraw.Draw(img)
    d.rectangle([14, 34, 24, 52], fill=(0, 150, 255, 255))    # blue
    d.rectangle([28, 22, 38, 52], fill=(175, 120, 255, 255))  # violet
    d.rectangle([42, 30, 52, 52], fill=(255, 90, 70, 255))    # red
    return img


mon = ClaudeMonitor()

# Shared state written by the poller thread, read by the UI.
state = {
    "claude":  {},      # get_usage_limits()
    "gpt":     {},      # get_codex_usage()
    "cstatus": "idle",
    "gstatus": "idle",
    "model":   "",
    "updated": 0.0,
    "err":     "",
}
_lock = threading.Lock()


def _poller():
    """Background thread: cheap status every 2s, usage (incl. network) every 30s."""
    last_usage = 0.0
    while True:
        try:
            cs = mon.get_status()
            gs = mon.get_codex_status()
            upd = {"cstatus": cs, "gstatus": gs, "err": ""}
            if time.monotonic() - last_usage > 30.0:
                upd["claude"] = mon.get_usage_limits() or {}
                upd["gpt"]    = mon.get_codex_usage() or {}
                try:
                    upd["model"] = mon.get_model() or ""
                except Exception:
                    pass
                last_usage = time.monotonic()
            upd["updated"] = time.time()
            with _lock:
                state.update(upd)
        except Exception as e:
            with _lock:
                state["err"] = str(e)
        time.sleep(2.0)


class Widget:
    def __init__(self, root):
        self.root = root
        root.overrideredirect(True)
        root.attributes("-topmost", True)
        root.configure(bg=BG)
        self._place()

        self.cv = tk.Canvas(root, width=W, height=H, bg=BG,
                            highlightthickness=1, highlightbackground="#24243A")
        self.cv.pack()

        # Drag via the header strip (top 32px).
        self.cv.bind("<Button-1>", self._press)
        self.cv.bind("<B1-Motion>", self._drag)
        self._dx = self._dy = 0
        self.hidden = False
        self.icon = None

        self.redraw()

    # ── tray / show / hide ────────────────────────────────────────────────────
    def setup_tray(self):
        menu = pystray.Menu(
            pystray.MenuItem("Show / Hide", self._tray_toggle, default=True),
            pystray.MenuItem("Exit", self._tray_exit),
        )
        self.icon = pystray.Icon("ai_usage", _tray_image(), "AI Usage", menu)
        threading.Thread(target=self.icon.run, daemon=True).start()

    def hide(self):
        self._save_pos()
        self.root.withdraw()
        self.hidden = True

    def show(self):
        self._place()                       # restore saved position
        self.root.deiconify()
        self.root.overrideredirect(True)    # re-assert frameless after deiconify
        self.root.attributes("-topmost", True)
        self.root.lift()
        self.hidden = False

    def toggle(self):
        self.hide() if not self.hidden else self.show()

    def quit(self):
        self._save_pos()
        try:
            if self.icon:
                self.icon.stop()
        except Exception:
            pass
        self.root.destroy()

    def _tray_toggle(self, icon=None, item=None):
        self.root.after(0, self.toggle)

    def _tray_exit(self, icon=None, item=None):
        self.root.after(0, self.quit)

    # ── window placement / drag ───────────────────────────────────────────────
    def _place(self):
        pos = None
        try:
            pos = POS_FILE.read_text().strip()
        except OSError:
            pass
        if pos and "," in pos:
            x, y = pos.split(",")[:2]
            self.root.geometry(f"{W}x{H}+{x}+{y}")
        else:
            sw = self.root.winfo_screenwidth()
            self.root.geometry(f"{W}x{H}+{sw - W - 24}+{48}")

    def _press(self, e):
        # x button hit-test (top-right 28px square)
        if e.x >= W - 30 and e.y <= 30:
            if _HAS_TRAY:
                self.hide()            # minimize to the tray (quit via tray > Exit)
            else:
                self._save_pos()
                self.root.destroy()
            return
        self._dx, self._dy = e.x, e.y

    def _drag(self, e):
        x = self.root.winfo_x() + e.x - self._dx
        y = self.root.winfo_y() + e.y - self._dy
        self.root.geometry(f"+{x}+{y}")

    def _save_pos(self):
        try:
            POS_FILE.write_text(f"{self.root.winfo_x()},{self.root.winfo_y()}")
        except OSError:
            pass

    # ── drawing helpers ───────────────────────────────────────────────────────
    def _round_rect(self, x0, y0, x1, y1, r, fill):
        self.cv.create_polygon(
            x0 + r, y0, x1 - r, y0, x1, y0, x1, y0 + r, x1, y1 - r, x1, y1,
            x1 - r, y1, x0 + r, y1, x0, y1, x0, y1 - r, x0, y0 + r, x0, y0,
            smooth=True, fill=fill, outline=fill)

    def _bar(self, y, label, pct, base_col):
        x0, x1 = 58, W - 52
        self._round_rect(x0, y, x1, y + 14, 6, BAR_BG)
        col = NEAR_LIMIT if pct >= 90 else base_col
        if pct > 0:
            fw = max(14, int((x1 - x0) * min(pct, 100) / 100))
            self._round_rect(x0, y, x0 + fw, y + 14, 6, col)
        self.cv.create_text(20, y + 7, text=label, fill=DIM,
                            font=("Segoe UI", 9), anchor="w")
        self.cv.create_text(W - 44, y + 7, text=f"{pct}%", fill=col,
                            font=("Segoe UI", 9, "bold"), anchor="w")

    def _dot(self, x, y, status):
        col = {"thinking": DOT_THINK, "waiting": DOT_WAIT,
               "idle": DOT_IDLE}.get(status, DOT_OFF)
        # pulse while thinking
        if status == "thinking" and int(time.time() * 2) % 2:
            col = "#6B5A1E"
        self.cv.create_oval(x - 5, y - 5, x + 5, y + 5, fill=col, outline="")

    def _provider(self, top, name, name_col, status, right_txt, bars, reset, have):
        """bars = list of (label, pct, color). Returns the y below this block."""
        self._dot(26, top + 8, status)
        self.cv.create_text(40, top + 8, text=name, fill=name_col,
                            font=("Segoe UI", 12, "bold"), anchor="w")
        if not have:
            self.cv.create_text(W - 16, top + 8, text="waiting for data",
                                fill=FAINT, font=("Segoe UI", 9), anchor="e")
            return top + 28
        if right_txt:
            self.cv.create_text(W - 16, top + 8, text=right_txt, fill=FAINT,
                                font=("Segoe UI", 9), anchor="e")
        y = top + 22
        for label, pct, col in bars:
            self._bar(y, label, pct, col)
            y += 22
        if reset:
            self.cv.create_text(W - 16, y, text=f"resets {reset}",
                                fill=FAINT, font=("Segoe UI", 8), anchor="e")
            y += 14
        return y

    # ── full redraw ───────────────────────────────────────────────────────────
    def redraw(self):
        with _lock:
            s = dict(state)
            c = dict(s["claude"]); g = dict(s["gpt"])

        self.cv.delete("all")
        # header
        self.cv.create_text(16, 16, text="AI Usage", fill=TXT,
                            font=("Segoe UI", 12, "bold"), anchor="w")
        self.cv.create_text(W - 15, 15, text="✕", fill=DIM,
                            font=("Segoe UI", 12), anchor="e")
        self.cv.create_line(14, 32, W - 14, 32, fill="#24243A")

        # Claude — 5h, Week, and the model-scoped weekly (Fable) if present
        model = s["model"].replace("claude-", "").replace("-20", " ") if s["model"] else ""
        claude_bars = [
            ("5h",   c.get("session_pct", 0),    CLAUDE_5H),
            ("Week", c.get("weekly_all_pct", 0), CLAUDE_WEEK),
        ]
        scoped_label = c.get("weekly_scoped_label")
        if scoped_label:
            claude_bars.append((scoped_label, c.get("weekly_scoped_pct", 0), CLAUDE_FABLE))
        y = self._provider(40, "Claude", CLAUDE_NAME, s["cstatus"], model,
                           claude_bars, c.get("weekly_reset_str", ""), bool(c))

        self.cv.create_line(14, y + 6, W - 14, y + 6, fill="#24243A")

        # ChatGPT — 5h, Week
        gpt_bars = [
            ("5h",   g.get("sess_pct", 0), GPT_5H),
            ("Week", g.get("week_pct", 0), GPT_WEEK),
        ]
        plan = (g.get("plan", "") or "").title()
        self._provider(y + 16, "ChatGPT", GPT_NAME, s["gstatus"], plan,
                       gpt_bars, g.get("week_reset", ""), bool(g))

        # footer
        if s["updated"]:
            t = time.strftime("%H:%M:%S", time.localtime(s["updated"]))
            foot = f"updated {t}"
        else:
            foot = "starting..."
        if not c and not g:
            foot += "   (re-login to Claude if blank)"
        self.cv.create_text(W / 2, H - 12, text=foot, fill=FAINT,
                            font=("Segoe UI", 8), anchor="c")

        self.root.after(500, self.redraw)


def main():
    threading.Thread(target=_poller, daemon=True).start()
    root = tk.Tk()
    root.title("AI Usage")
    w = Widget(root)
    if _HAS_TRAY:
        w.setup_tray()
    root.mainloop()


if __name__ == "__main__":
    main()
