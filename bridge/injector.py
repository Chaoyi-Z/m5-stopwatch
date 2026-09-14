"""
Injects the transcript into whichever input box currently has keyboard focus.

HOW TO USE:
  1. Click into the input box where you want the text (Claude Code panel in VS Code,
     Claude.ai in a browser, the standalone Claude app — anything works).
  2. Press the Blue button on the StopWatch.
  3. The transcript is pasted there via Ctrl+V.

You only need to click once per session. As long as that input keeps focus between
recordings, every Blue press lands in the same place.
"""
import time
import pyperclip
import pyautogui


class TextInjector:
    def inject(self, text: str) -> bool:
        pyperclip.copy(text)
        time.sleep(0.05)          # let clipboard settle
        pyautogui.hotkey("ctrl", "v")
        print(f"[Injector] Pasted {len(text)} chars into focused window.")
        return True

    def submit(self) -> bool:
        """Press Enter in the focused window (sends the pasted message)."""
        pyautogui.press("enter")
        print("[Injector] Pressed Enter in focused window.")
        return True

    def allow(self) -> bool:
        """Approve a Claude Code permission prompt (Ctrl+Enter = 'Allow once')."""
        pyautogui.hotkey("ctrl", "enter")
        print("[Injector] Sent Allow (Ctrl+Enter) to focused window.")
        return True

    def deny(self) -> bool:
        """Reject / cancel a Claude Code permission prompt (Escape)."""
        pyautogui.press("escape")
        print("[Injector] Sent Deny (Escape) to focused window.")
        return True
