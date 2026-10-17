"""iPhone Remote — see and control your iPhone from Windows through the M5 StopWatch.

    iPhone ──AirPlay mirror──► receiver window on the PC (UxPlay / AirServer / LonelyScreen)
    this app ──transparent overlay on that window── clicks/keys ──USB serial──► watch
    watch ──Bluetooth LE mouse + keyboard──► iPhone (AssistiveTouch pointer)

Clicks are sent as "home the pointer to the top-left corner, then move a calibrated
number of counts", so a click on the mirror lands on the same spot on the phone.
Run Calibrate once (and again if you change AssistiveTouch Tracking Speed).

Requires: pyserial, pywin32.  Run:  python iphone_remote.py
"""
import ctypes
import json
import os
import queue
import subprocess
import threading
import time
import tkinter as tk
import webbrowser
from pathlib import Path
from tkinter import messagebox

import serial
import serial.tools.list_ports
import win32api
import win32con
import win32gui

ctypes.windll.shcore.SetProcessDpiAwareness(2)   # Tk coordinates = physical pixels

CONFIG_PATH = Path(__file__).with_name("remote_config.json")
DEFAULTS = {
    "mirror_title": "",          # substring of the AirPlay receiver window title
    "full_w": 0,                 # pointer counts to cross the full screen width (calibrated)
    "full_h": 0,                 # ... and height
    "pace_step": 16,             # counts per paced report (firmware "p" command)
    "pace_ms": 8,                # ms between paced reports
    "ctrl_as_cmd": True,         # PC Ctrl → iPhone ⌘ (so Ctrl+C / Ctrl+V / Ctrl+A work)
    "scroll_step": 3,            # wheel units per mouse-wheel notch
    "phone_w": 1206,             # iPhone 16 Pro native resolution (portrait), for letterboxing
    "phone_h": 2622,
    "start_receiver": True,      # launch UxPlay (AirPlay receiver) with this app
    "uxplay_exe": "C:/msys64/ucrt64/bin/uxplay.exe",
    "uxplay_args": ["-n", "iPhone Mirror", "-nh", "-p", "-vs", "d3d11videosink", "-as", "wasapisink"],
}
KNOWN_RECEIVERS = ("UxPlay", "AirServer", "LonelyScreen", "Direct3D11 renderer",
                   "Direct3D renderer", "OpenGL renderer", "AirPlay")

# Windows virtual-key → HID keyboard usage
VK_TO_HID = {
    **{0x41 + i: 0x04 + i for i in range(26)},                 # A–Z
    **{0x31 + i: 0x1E + i for i in range(9)}, 0x30: 0x27,      # 1–9, 0
    **{0x70 + i: 0x3A + i for i in range(12)},                 # F1–F12
    0x0D: 0x28, 0x1B: 0x29, 0x08: 0x2A, 0x09: 0x2B, 0x20: 0x2C,
    0xBD: 0x2D, 0xBB: 0x2E, 0xDB: 0x2F, 0xDD: 0x30, 0xDC: 0x31, 0xBA: 0x33,
    0xDE: 0x34, 0xC0: 0x35, 0xBC: 0x36, 0xBE: 0x37, 0xBF: 0x38, 0x14: 0x39,
    0x2E: 0x4C, 0x24: 0x4A, 0x23: 0x4D, 0x21: 0x4B, 0x22: 0x4E,
    0x27: 0x4F, 0x25: 0x50, 0x28: 0x51, 0x26: 0x52,
}
MOD_CTRL, MOD_SHIFT, MOD_ALT, MOD_GUI = 0x01, 0x02, 0x04, 0x08

# Consumer-control usages iOS understands
CC_HOME, CC_VOL_UP, CC_VOL_DOWN, CC_PLAY, CC_KEYBOARD = 0x223, 0xE9, 0xEA, 0xCD, 0x1AE


def load_config():
    cfg = dict(DEFAULTS)
    try:
        cfg.update(json.loads(CONFIG_PATH.read_text()))
    except Exception:
        pass
    return cfg


def save_config(cfg):
    CONFIG_PATH.write_text(json.dumps(cfg, indent=2))


# ── Watch link (USB serial) ──────────────────────────────────────────────────
class Watch:
    def __init__(self, on_status):
        self.on_status = on_status
        self.ser = None
        self.port = None
        self.ble = False
        self.conn = False
        self.q = queue.Queue()
        self.alive = True
        threading.Thread(target=self._run, daemon=True).start()

    def send(self, line):
        self.q.put(line)

    def _find_port(self):
        for p in serial.tools.list_ports.comports():
            if p.vid == 0x303A:          # Espressif native USB
                return p.device
        return None

    def _open(self):
        port = self._find_port()
        if not port:
            return False
        s = serial.Serial()
        s.port, s.baudrate, s.timeout, s.write_timeout = port, 115200, 0, 1
        s.dtr, s.rts = True, False        # don't reset the chip on open
        s.open()
        self.ser, self.port = s, port
        self._write("ble 1")
        return True

    def _write(self, line):
        self.ser.write((line + "\n").encode("ascii", "ignore"))

    def _run(self):
        buf, last_poll = b"", 0
        while self.alive:
            try:
                if self.ser is None:
                    if not self._open():
                        self.on_status(None, False, False)
                        time.sleep(2)
                        continue
                try:
                    line = self.q.get(timeout=0.02)
                    self._write(line)
                except queue.Empty:
                    pass
                if time.time() - last_poll > 1.5:
                    self._write("?")
                    last_poll = time.time()
                buf += self.ser.read(256)
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    txt = raw.decode(errors="replace").strip()
                    if txt.startswith("HID ble="):
                        kv = dict(p.split("=") for p in txt[4:].split())
                        self.ble, self.conn = kv.get("ble") == "1", kv.get("conn") == "1"
                        self.on_status(self.port, self.ble, self.conn)
            except (serial.SerialException, OSError):
                try:
                    self.ser.close()
                except Exception:
                    pass
                self.ser = None
                self.on_status(None, False, False)
                time.sleep(1)

    def close(self):
        if self.ser:
            try:
                self._write("ble 0")
                time.sleep(0.2)
                self.ser.close()
            except Exception:
                pass
        self.alive = False


# ── App ──────────────────────────────────────────────────────────────────────
class App:
    def __init__(self):
        self.cfg = load_config()
        self.root = tk.Tk()
        self.root.title("iPhone Remote")
        self.root.attributes("-topmost", True)
        self.root.resizable(False, False)
        self.root.protocol("WM_DELETE_WINDOW", self.quit)

        self.mirror_hwnd = None
        self.control_on = True
        self.calibrating = False
        self.picking = False
        self.video = None                 # (x, y, w, h) of the phone image on screen
        self.keys_down = []               # HID usages currently held
        self.mods_down = set()            # VK codes of held modifiers
        self.drag_last = None
        self.drag_acc = [0.0, 0.0]

        self.receiver = None              # UxPlay process we started (stopped on quit)
        self._build_panel()
        self._build_overlay()
        self._start_receiver()
        self.watch = Watch(lambda *a: self.root.after(0, self._watch_status, *a))
        self.watch.send(f"p {self.cfg['pace_step']} {self.cfg['pace_ms']}")
        self._find_mirror()
        self._track()
        self._flush_drag()

    # ── UI ───────────────────────────────────────────────────────────────────
    def _build_panel(self):
        r = self.root
        pad = dict(padx=4, pady=3, sticky="ew")
        self.lbl_watch = tk.Label(r, text="Watch: searching…", anchor="w")
        self.lbl_watch.grid(row=0, column=0, columnspan=4, **pad)
        self.lbl_mirror = tk.Label(r, text="Mirror: not found", anchor="w")
        self.lbl_mirror.grid(row=1, column=0, columnspan=4, **pad)

        self.btn_ctl = tk.Button(r, text="Control: ON", command=self.toggle_control, width=12)
        self.btn_ctl.grid(row=2, column=0, **pad)
        tk.Button(r, text="Pick mirror", command=self.pick_mirror).grid(row=2, column=1, **pad)
        tk.Button(r, text="Calibrate", command=self.start_calibration).grid(row=2, column=2, **pad)
        tk.Button(r, text="Home", command=lambda: self.consumer(CC_HOME)).grid(row=2, column=3, **pad)

        tk.Button(r, text="Paste PC text → iPhone", command=self.paste_to_phone).grid(
            row=3, column=0, columnspan=2, **pad)
        tk.Button(r, text="iPhone → PC (ClipBridge)", command=self.open_clipbridge).grid(
            row=3, column=2, columnspan=2, **pad)

        tk.Button(r, text="Search", command=lambda: self.tap(MOD_GUI, 0x2C)).grid(row=4, column=0, **pad)
        tk.Button(r, text="Keyboard", command=lambda: self.consumer(CC_KEYBOARD)).grid(row=4, column=1, **pad)
        tk.Button(r, text="Vol −", command=lambda: self.consumer(CC_VOL_DOWN)).grid(row=4, column=2, **pad)
        tk.Button(r, text="Vol +", command=lambda: self.consumer(CC_VOL_UP)).grid(row=4, column=3, **pad)

        self.var_ctrl = tk.BooleanVar(value=self.cfg["ctrl_as_cmd"])
        tk.Checkbutton(r, text="Ctrl = ⌘", variable=self.var_ctrl,
                       command=self._save_ctrl).grid(row=5, column=0, **pad)
        tk.Button(r, text="Forget pairing", command=self.forget).grid(row=5, column=3, **pad)
        self.lbl_hint = tk.Label(r, text="", anchor="w", fg="#555", justify="left", wraplength=420)
        self.lbl_hint.grid(row=6, column=0, columnspan=4, **pad)
        self._hint()

    def _build_overlay(self):
        ov = self.overlay = tk.Toplevel(self.root)
        ov.overrideredirect(True)
        ov.attributes("-topmost", True)
        ov.attributes("-alpha", 0.01)      # invisible, but still receives clicks
        ov.configure(bg="black", cursor="crosshair")
        ov.withdraw()
        ov.bind("<ButtonPress-1>", self.on_down)
        ov.bind("<B1-Motion>", self.on_drag)
        ov.bind("<ButtonRelease-1>", self.on_up)
        ov.bind("<ButtonPress-3>", self.on_right)
        ov.bind("<MouseWheel>", self.on_wheel)
        ov.bind("<Shift-MouseWheel>", lambda e: self.on_wheel(e, pan=True))
        ov.bind("<KeyPress>", self.on_key)
        ov.bind("<KeyRelease>", self.on_key_up)
        ov.bind("<FocusOut>", lambda e: self.release_keys())

    def _hint(self, extra=""):
        cal = "calibrated" if self.cfg["full_w"] else "NOT calibrated — press Calibrate"
        self.lbl_hint.config(text=(extra or
            f"Click/drag on the mirror = tap/swipe, right-click = long-press, wheel = scroll, "
            f"type while the mirror is focused. Pointer {cal}."))

    def _save_ctrl(self):
        self.cfg["ctrl_as_cmd"] = self.var_ctrl.get()
        save_config(self.cfg)

    def _watch_status(self, port, ble, conn):
        if not port:
            self.lbl_watch.config(text="Watch: not found (plug in the StopWatch)", fg="#b00")
        elif conn:
            self.lbl_watch.config(text=f"Watch: {port} — iPhone connected", fg="#080")
        elif ble:
            self.lbl_watch.config(text=f"Watch: {port} — waiting for iPhone (pair 'StopWatch Remote')", fg="#a60")
        else:
            self.lbl_watch.config(text=f"Watch: {port} — Bluetooth off", fg="#a60")

    # ── AirPlay receiver (UxPlay) ────────────────────────────────────────────
    def _start_receiver(self):
        exe = self.cfg["uxplay_exe"]
        if not self.cfg["start_receiver"] or not Path(exe).exists():
            return
        running = subprocess.run(["tasklist", "/FI", "IMAGENAME eq uxplay.exe", "/NH"],
                                 capture_output=True, text=True,
                                 creationflags=subprocess.CREATE_NO_WINDOW).stdout
        if "uxplay.exe" in running:
            return
        env = dict(os.environ)
        env["PATH"] = str(Path(exe).parent) + os.pathsep + env.get("PATH", "")   # GStreamer DLLs
        self.receiver = subprocess.Popen([exe] + self.cfg["uxplay_args"], env=env,
                                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                         creationflags=subprocess.CREATE_NO_WINDOW)

    # ── Mirror window tracking ───────────────────────────────────────────────
    def _find_mirror(self):
        want = self.cfg["mirror_title"]
        found = []

        def cb(h, _):
            if win32gui.IsWindowVisible(h):
                t = win32gui.GetWindowText(h)
                if t and ((want and want in t) or (not want and any(k in t for k in KNOWN_RECEIVERS))):
                    found.append(h)
        win32gui.EnumWindows(cb, None)
        found = [h for h in found if h != self._ov_hwnd() and h != self._panel_hwnd()]
        self.mirror_hwnd = found[0] if found else None

    def pick_mirror(self):
        self.picking = True
        self.lbl_mirror.config(text="Mirror: hover over the mirror window… 3")
        def step(n):
            if n:
                self.lbl_mirror.config(text=f"Mirror: hover over the mirror window… {n}")
                self.root.after(1000, step, n - 1)
                return
            self.picking = False
            h = win32gui.GetAncestor(win32gui.WindowFromPoint(win32api.GetCursorPos()), 2)
            if h in (self._ov_hwnd(), self._panel_hwnd()):
                self.lbl_mirror.config(text="Mirror: that's this app — try again")
                return
            self.mirror_hwnd = h
            self.cfg["mirror_title"] = win32gui.GetWindowText(h)
            save_config(self.cfg)
        step(3)

    def _ov_hwnd(self):
        return win32gui.GetAncestor(self.overlay.winfo_id(), 2)

    def _panel_hwnd(self):
        return win32gui.GetAncestor(self.root.winfo_id(), 2)

    def _video_rect(self):
        """Screen rect of the phone image inside the receiver's client area (letterbox-aware)."""
        h = self.mirror_hwnd
        l, t, r, b = win32gui.GetClientRect(h)
        x0, y0 = win32gui.ClientToScreen(h, (0, 0))
        cw, ch = r - l, b - t
        if cw < 50 or ch < 50:
            return None
        pw, ph = self.cfg["phone_w"], self.cfg["phone_h"]
        if cw > ch:                                    # landscape mirror
            pw, ph = ph, pw
        scale = min(cw / pw, ch / ph)
        vw, vh = int(pw * scale), int(ph * scale)
        return (x0 + (cw - vw) // 2, y0 + (ch - vh) // 2, vw, vh)

    def _track(self):
        try:
            if self.mirror_hwnd and not win32gui.IsWindow(self.mirror_hwnd):
                self.mirror_hwnd = None
            if not self.mirror_hwnd:
                self._find_mirror()
            show = False
            if self.picking:
                pass
            elif self.mirror_hwnd:
                title = win32gui.GetWindowText(self.mirror_hwnd)
                self.lbl_mirror.config(text=f"Mirror: {title[:50]}", fg="#080")
                fg = win32gui.GetForegroundWindow()
                visible = not win32gui.IsIconic(self.mirror_hwnd)
                focused = fg in (self.mirror_hwnd, self._ov_hwnd(), self._panel_hwnd())
                self.video = self._video_rect() if visible else None
                show = self.control_on and visible and focused and self.video is not None
            else:
                rx = "receiver running" if self.receiver and self.receiver.poll() is None else "start an AirPlay receiver"
                self.lbl_mirror.config(text=f"Mirror: waiting — {rx}; on the iPhone: "
                                            "Control Center → Screen Mirroring → iPhone Mirror", fg="#a60")
            if show:
                x, y, w, h = self.video
                self.overlay.geometry(f"{w}x{h}+{x}+{y}")
                if self.overlay.state() == "withdrawn":
                    self.overlay.deiconify()
                    self.overlay.attributes("-topmost", True)
            elif self.overlay.state() != "withdrawn":
                self.release_keys()
                self.overlay.withdraw()
        except Exception:
            pass
        self.root.after(100, self._track)

    def toggle_control(self):
        self.control_on = not self.control_on
        self.btn_ctl.config(text=f"Control: {'ON' if self.control_on else 'OFF'}")

    # ── Pointer ──────────────────────────────────────────────────────────────
    def _norm(self, e):
        x, y, w, h = self.video
        return min(max(e.x / w, 0.0), 1.0), min(max(e.y / h, 0.0), 1.0)

    def _goto(self, u, v):
        if not self.cfg["full_w"]:
            self._hint("Calibrate first: press Calibrate, then click where the pointer stops.")
            return False
        self.watch.send(f"g {round(u * self.cfg['full_w'])} {round(v * self.cfg['full_h'])}")
        return True

    def on_down(self, e):
        self.overlay.focus_force()
        u, v = self._norm(e)
        if self.calibrating:
            self._finish_calibration(u, v)
            return
        if self._goto(u, v):
            self.watch.send("b 1")
            self.drag_last = (e.x, e.y)
            self.drag_acc = [0.0, 0.0]

    def on_drag(self, e):
        if self.drag_last is None or not self.video:
            return
        _, _, w, h = self.video
        dx, dy = e.x - self.drag_last[0], e.y - self.drag_last[1]
        self.drag_last = (e.x, e.y)
        self.drag_acc[0] += dx / w * self.cfg["full_w"]
        self.drag_acc[1] += dy / h * self.cfg["full_h"]

    def _flush_drag(self):
        ix, iy = int(self.drag_acc[0]), int(self.drag_acc[1])
        if ix or iy:
            self.drag_acc[0] -= ix
            self.drag_acc[1] -= iy
            self.watch.send(f"m {ix} {iy}")
        self.root.after(16, self._flush_drag)

    def on_up(self, e):
        if self.drag_last is None:
            return
        self.drag_last = None
        self._flush_now()
        self.watch.send("b 0")

    def _flush_now(self):
        ix, iy = int(self.drag_acc[0]), int(self.drag_acc[1])
        self.drag_acc = [0.0, 0.0]
        if ix or iy:
            self.watch.send(f"m {ix} {iy}")

    def on_right(self, e):
        self.overlay.focus_force()
        if self._goto(*self._norm(e)):
            self.watch.send("c 2")

    def on_wheel(self, e, pan=False):
        n = (1 if e.delta > 0 else -1) * self.cfg["scroll_step"]
        self.watch.send(f"m 0 0 0 {n}" if pan else f"m 0 0 {n}")

    # ── Calibration ──────────────────────────────────────────────────────────
    CAL_COUNTS = 200

    def start_calibration(self):
        if not self.watch.conn:
            messagebox.showinfo("Calibrate", "Connect the iPhone first (Bluetooth → StopWatch Remote).")
            return
        if not self.video:
            messagebox.showinfo("Calibrate", "Open the mirror window first (iPhone Screen Mirroring).")
            return
        self.calibrating = True
        self.watch.send(f"g {self.CAL_COUNTS} {self.CAL_COUNTS}")
        self._hint("CALIBRATING: click exactly on the tip of the iPhone pointer in the mirror. "
                   "(If the pointer is pinned to the screen edge, lower Tracking Speed in iPhone "
                   "Settings → Accessibility → Touch → AssistiveTouch, then Calibrate again.)")
        if self.mirror_hwnd:
            try:
                win32gui.SetForegroundWindow(self.mirror_hwnd)
            except Exception:
                pass

    def _finish_calibration(self, u, v):
        self.calibrating = False
        if u < 0.03 or v < 0.03 or u > 0.97 or v > 0.97:
            self._hint("Calibration click was too close to an edge — try Calibrate again.")
            return
        self.cfg["full_w"] = round(self.CAL_COUNTS / u)
        self.cfg["full_h"] = round(self.CAL_COUNTS / v)
        save_config(self.cfg)
        self._hint(f"Calibrated: full width = {self.cfg['full_w']} counts, "
                   f"height = {self.cfg['full_h']} counts. Click anywhere to test.")

    # ── Keyboard ─────────────────────────────────────────────────────────────
    def _mod_mask(self):
        m = 0
        for vk in self.mods_down:
            if vk == win32con.VK_SHIFT:
                m |= MOD_SHIFT
            elif vk == win32con.VK_CONTROL:
                m |= MOD_GUI if self.cfg["ctrl_as_cmd"] else MOD_CTRL
            elif vk == win32con.VK_MENU:
                m |= MOD_ALT
        return m

    def _send_keys(self):
        self.watch.send("k " + " ".join(str(x) for x in [self._mod_mask()] + self.keys_down[:6]))

    def on_key(self, e):
        vk = e.keycode
        if vk in (win32con.VK_SHIFT, win32con.VK_CONTROL, win32con.VK_MENU):
            if vk not in self.mods_down:
                self.mods_down.add(vk)
                self._send_keys()
            return "break"
        # Ctrl+Shift+V = type the PC clipboard into the iPhone
        if vk == 0x56 and {win32con.VK_CONTROL, win32con.VK_SHIFT} <= self.mods_down:
            self.release_keys()
            self.paste_to_phone()
            return "break"
        usage = VK_TO_HID.get(vk)
        if usage and usage not in self.keys_down:      # ignore Windows auto-repeat
            self.keys_down.append(usage)
            self._send_keys()
        return "break"

    def on_key_up(self, e):
        vk = e.keycode
        if vk in self.mods_down:
            self.mods_down.discard(vk)
        usage = VK_TO_HID.get(vk)
        if usage in self.keys_down:
            self.keys_down.remove(usage)
        self._send_keys()
        return "break"

    def release_keys(self):
        if self.keys_down or self.mods_down:
            self.keys_down, self.mods_down = [], set()
            self.watch.send("k 0")

    def tap(self, mod, usage):
        self.watch.send(f"K {mod} {usage}")

    def consumer(self, usage):
        self.watch.send(f"u {usage}")

    # ── Clipboard ────────────────────────────────────────────────────────────
    def paste_to_phone(self):
        try:
            text = self.root.clipboard_get()
        except tk.TclError:
            self._hint("PC clipboard has no text. For images use ClipBridge.")
            return
        text = text.replace("\r\n", "\n")
        clean = "".join(c for c in text if c in "\n\t" or 32 <= ord(c) < 127)
        skipped = len(text) - len(clean)
        esc = clean.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t")
        # firmware lines are capped at 1024 chars; split without breaking an escape
        chunk = ""
        for ch in esc:
            chunk += ch
            if len(chunk) >= 400 and not chunk.endswith("\\"):
                self.watch.send("t " + chunk)
                chunk = ""
        if chunk:
            self.watch.send("t " + chunk)
        msg = f"Typing {len(clean)} characters into the iPhone (~{len(clean) * 16 // 1000 + 1}s)."
        if skipped:
            msg += f" {skipped} non-ASCII characters skipped — use ClipBridge for those."
        self._hint(msg)

    def open_clipbridge(self):
        webbrowser.open("http://localhost:48765/")

    def forget(self):
        if messagebox.askyesno("Forget pairing",
                               "Delete the watch's Bluetooth pairing? Also remove "
                               "'StopWatch Remote' in iPhone Settings → Bluetooth, then pair again."):
            self.watch.send("forget")

    def quit(self):
        self.release_keys()
        self.watch.close()
        if self.receiver and self.receiver.poll() is None:
            self.receiver.terminate()
        self.root.destroy()

    def run(self):
        self.root.mainloop()


if __name__ == "__main__":
    App().run()
