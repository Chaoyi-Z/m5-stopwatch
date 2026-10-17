# iPhone Remote

See and control the iPhone from Windows, with the StopWatch acting as a Bluetooth mouse + keyboard.

```
iPhone ──AirPlay Screen Mirroring──► receiver window on the PC (UxPlay / AirServer / LonelyScreen)
iphone_remote.py ── invisible overlay on that window ── USB serial (COM7) ──► StopWatch
StopWatch ──Bluetooth LE mouse/keyboard ("StopWatch Remote")──► iPhone
```

The watch keeps all its Claude-companion features. Bluetooth is only on while this app runs
(it sends `ble 1` on start and `ble 0` on exit), because BLE forces WiFi into modem-sleep,
which MIT's access points sometimes punish with a reconnect.

## One-time setup

1. **iPhone pairing:** start the app (`iPhone Remote.bat`), then on the iPhone open
   Settings → Bluetooth and tap **StopWatch Remote**.
2. **Pointer:** Settings → Accessibility → Touch → **AssistiveTouch** on (iOS only shows a mouse
   pointer with AssistiveTouch). Tracking Speed there changes the pointer gain; recalibrate after changing it.
3. **Mirroring:** run an AirPlay receiver on the PC, then on the iPhone Control Center →
   Screen Mirroring → pick the PC. The PC and phone must reach each other on the network.
4. **Calibrate:** with the mirror open, press **Calibrate**, then click on the pointer tip in the mirror.

## Use

| On the mirror window | On the iPhone |
|---|---|
| click / drag | tap / swipe |
| right-click | secondary click (long-press menu) |
| wheel, Shift+wheel | scroll, horizontal scroll |
| typing (mirror focused) | keyboard input; Ctrl acts as ⌘ (Ctrl+C/V/A/Z) unless unticked |
| Ctrl+Shift+V | types the PC clipboard text into the iPhone |

Buttons: Home, Search (⌘Space), on-screen Keyboard toggle, Vol ±, Control ON/OFF (lets clicks reach
the receiver window), Forget pairing.

**Clipboard:** PC → iPhone text is typed by the watch (ASCII only, ~60 chars/s). Images, non-ASCII
text and iPhone → PC go through ClipBridge (`Github\ClipBridge`, http://localhost:48765).

Settings are kept in `remote_config.json` (pointer calibration, mirror window title, pacing).

## Serial protocol

One line per command on the watch's USB serial port; also accepted over the bridge WebSocket as
`HID:<line>`. See the header of `firmware/src/ble_hid.h`.
