#include <M5Unified.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <esp_wifi.h>
#include "config.h"
#include "ui_status.h"
#include "ui_tokens.h"
#include "ui_voice.h"
#include "ui_memo.h"

// ── Screens ───────────────────────────────────────────────────────────────────
enum Screen { SCR_STATUS = 0, SCR_TOKENS = 1, SCR_MEMO = 2 };
Screen currentScreen = SCR_STATUS;

// ── Display canvas ────────────────────────────────────────────────────────────
M5Canvas canvas(&M5.Display);

// ── WebSocket ─────────────────────────────────────────────────────────────────
WebSocketsClient ws;
bool wsConnected = false;

// ── Shared state ──────────────────────────────────────────────────────────────
String claudeStatus     = "idle";
bool   chimePending     = false;   // beep when Claude finishes thinking
bool   wakeForThink     = false;   // wake the screen once when Claude starts working
int    tokenIn        = 0;
int    tokenOut       = 0;
float  cost           = 0.0f;
int    dailyIn        = 0;
int    dailyOut       = 0;
float  dailyCost      = 0.0f;
// Anthropic account usage limits (from USAGE: message)
int    usageSessPct   = -1;   // -1 = not yet received
int    usageSessResetMin = 0;
int    usageWeekAllPct   = 0;
int    usageWeekSonPct   = 0;
String usageWeekReset    = "";
String currentModel      = "";   // friendly model name, e.g. "Opus 4.8"
int    battPct        = 0;
bool   recording      = false;
bool   transcribing   = false;   // true while waiting for STT result from bridge
String lastTranscript = "";
bool   injected       = false;   // transcript already sent to PC → next Blue = Enter
// Attention notification (permission prompt etc.) pushed via NOTIFY:/NOTIFYP:
bool          notifyPending     = false;
String        notifyMsg         = "";
unsigned long notifyShownMs     = 0;
bool          permissionPending = false;   // a permission prompt awaits → Blue = Allow
unsigned long permissionMs      = 0;
int    weatherCode    = -1;
int    weatherTemp    = 20;

unsigned long sessionStartMs  = 0;
unsigned long lastHeartbeatMs = 0;
unsigned long lastDrawMs      = 0;
unsigned long recordStartMs   = 0;
uint32_t      animFrame       = 0;

// ── Display sleep ─────────────────────────────────────────────────────────────
static unsigned long _lastActivityMs = 0;
static bool          _displayAsleep  = false;

// Screen off ⇒ also drop the CPU to 80 MHz (the floor that keeps WiFi alive) to
// save power; screen on ⇒ full 240 MHz. Routed through these helpers so a wake
// can never leave the chip stuck slow. Idempotent.
static void wakeDisplay() {
    if (!_displayAsleep) return;
    _displayAsleep = false;
    setCpuFrequencyMhz(240);
    M5.Display.setBrightness(DISPLAY_BRIGHTNESS);
}
static void sleepDisplay() {
    if (_displayAsleep) return;
    _displayAsleep = true;
    M5.Display.setBrightness(0);
    setCpuFrequencyMhz(80);
}

// ── Audio streaming (STT: device mic → bridge live) ──────────────────────────
static const uint32_t AUDIO_SR    = 16000;
static const size_t   AUDIO_CHUNK = 256;

static size_t  audioQueued = 0;
float          micLevel    = 0.0f;

// ── Voice memo (local record → PSRAM → speaker playback) ─────────────────────
static const size_t MEMO_MAX_SAMPLES = AUDIO_SR * 15;  // 15 seconds
static int16_t*     _memoBuf         = nullptr;
static size_t       _memoSamples     = 0;
bool                memoRecording    = false;
bool                memoPlaying      = false;
unsigned long       memoPlayStartMs  = 0;

static const int MEMO_VOL_STEPS[] = {160, 200, 230, 255};
static const int MEMO_VOL_COUNT   = 4;
int              memoVolIdx        = 3;   // default: max

// ── Audio helpers ─────────────────────────────────────────────────────────────
// The ES8311 codec (I2C addr 0x18) is on GPIO47/48, which M5Unified maps to
// In_I2C for the StopWatch board. Both helpers overwrite registers set by
// M5Unified's built-in enable callbacks to match xiaozhi-esp32's settings.

static void speakerOn() {
    M5.Speaker.begin();
    M5.Speaker.setVolume(255);
    // ES8311 DAC volume: 0xBF = library default (0 dB), 0xFF = max (+32 dB, 0.5 dB/step).
    M5.In_I2C.writeRegister8(0x18, 0x32, 0xFF, 100000);
}

static void micOn() {
    M5.Speaker.end();
    M5.Mic.begin();
    // Boost ES8311 ADC hardware PGA to 30 dB (5 × 6 dB steps).
    // This amplifies the signal before A/D conversion → lower noise floor than
    // relying on M5Unified's default 0 dB PGA + software magnification.
    // Register 0x17 (ADC digital gain) is left at M5Unified's default 0xFF —
    // writing it to 0xBF was found to silence the speaker on subsequent playback.
    M5.In_I2C.writeRegister8(0x18, 0x14, 0x15, 100000);  // PGA = 30 dB
}

// ── WebSocket message parser ──────────────────────────────────────────────────
void handleMessage(const String& msg) {
    if (msg.startsWith("STATUS:")) {
        String newStatus = msg.substring(7);
        newStatus.trim();
        // Queue the done-chime exactly once on the thinking → (waiting/idle) edge.
        if (claudeStatus == "thinking" && newStatus != "thinking") {
            chimePending = true;
        }
        // Wake the screen once when Claude starts working (shows the home spinner).
        if (claudeStatus != "thinking" && newStatus == "thinking") {
            wakeForThink = true;
        }
        claudeStatus = newStatus;
    } else if (msg.startsWith("TOKENS:")) {
        auto extract = [&](const char* key) -> String {
            int idx = msg.indexOf(key);
            if (idx < 0) return "0";
            idx += strlen(key);
            int end = msg.indexOf(',', idx);
            return (end < 0) ? msg.substring(idx) : msg.substring(idx, end);
        };
        tokenIn  = extract("in=").toInt();
        tokenOut = extract("out=").toInt();
        cost     = extract("cost=").toFloat();
        dailyIn  = extract("din=").toInt();
        dailyOut = extract("dout=").toInt();
        dailyCost = extract("dcost=").toFloat();
    } else if (msg.startsWith("TRANSCRIPT:")) {
        lastTranscript = msg.substring(11);
        lastTranscript.trim();
        transcribing = false;
        recording    = false;
        injected     = false;   // fresh transcript → first Blue press injects
        if (lastTranscript.length() == 0) {
            lastTranscript = "(couldn't hear — try again)";
        }
    } else if (msg.startsWith("USAGE:")) {
        auto extractU = [&](const char* key) -> String {
            int idx = msg.indexOf(key);
            if (idx < 0) return "0";
            idx += strlen(key);
            int end = msg.indexOf(',', idx);
            return (end < 0) ? msg.substring(idx) : msg.substring(idx, end);
        };
        usageSessPct      = extractU("sess=").toInt();
        usageSessResetMin = extractU("sreset=").toInt();
        usageWeekAllPct   = extractU("wall=").toInt();
        usageWeekSonPct   = extractU("wson=").toInt();
        usageWeekReset    = extractU("wreset=");
    } else if (msg.startsWith("MODEL:")) {
        currentModel = msg.substring(6);
        currentModel.trim();
    } else if (msg.startsWith("NOTIFYP:")) {
        notifyMsg = msg.substring(8);
        notifyMsg.trim();
        notifyPending     = true;   // alert fired in loop()
        permissionPending = true;   // Blue button now approves the prompt
        permissionMs      = millis();
    } else if (msg.startsWith("NOTIFY:")) {
        notifyMsg = msg.substring(7);
        notifyMsg.trim();
        notifyPending = true;   // alert fired in loop()
    } else if (msg.startsWith("WEATHER:")) {
        auto extractW = [&](const char* key) -> String {
            int idx = msg.indexOf(key);
            if (idx < 0) return "0";
            idx += strlen(key);
            int end = msg.indexOf(',', idx);
            return (end < 0) ? msg.substring(idx) : msg.substring(idx, end);
        };
        weatherTemp = extractW("temp=").toInt();
        weatherCode = extractW("code=").toInt();
    }
}

// ── WebSocket event handler ───────────────────────────────────────────────────
void onWsEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_CONNECTED:
            wsConnected = true;
            break;
        case WStype_DISCONNECTED:
            wsConnected = false;
            if (recording) {
                M5.Mic.end();
                speakerOn();
            }
            recording    = false;
            transcribing = false;
            break;
        case WStype_TEXT:
            handleMessage(String((char*)payload));
            break;
        default:
            break;
    }
}

// ── Stop recording — audio was already streamed live, just close the session ──
void stopAndSendAudio() {
    if (!recording) return;
    recording = false;
    micLevel  = 0.0f;

    if (wsConnected) {
        // Drain the last in-flight DMA chunk before signalling stop
        unsigned long t = millis();
        while (M5.Mic.isRecording() > 0 && millis() - t < 120) {
            ws.loop();
            delay(1);
        }
        ws.sendTXT("CMD:RECORD_STOP");
        transcribing = true;
    }

    // Release mic I2S pins → speaker reclaims them for chime/playback
    M5.Mic.end();
    speakerOn();
}

// ── Heartbeat ─────────────────────────────────────────────────────────────────
void sendHeartbeat() {
    battPct = M5.Power.getBatteryLevel();
    ws.sendTXT("CMD:HEARTBEAT:batt=" + String(battPct));
}

// ── Physical buttons ──────────────────────────────────────────────────────────
void handleButtons() {
    bool anyBtn = M5.BtnA.wasPressed() || M5.BtnA.wasReleased() || M5.BtnB.wasPressed();
    if (anyBtn) {
        _lastActivityMs = millis();
        if (_displayAsleep) {
            wakeDisplay();
            return;  // first press just wakes the screen
        }
    }

    // Permission prompt pending → Blue approves, Yellow denies (any screen), for a window
    if (permissionPending && (millis() - permissionMs < PERMISSION_WINDOW_MS)) {
        if (M5.BtnB.wasPressed() || M5.BtnA.wasPressed()) {
            ws.sendTXT(M5.BtnB.wasPressed() ? "CMD:ALLOW" : "CMD:DENY");
            permissionPending = false;
            notifyMsg = "";                       // clear the banner
            M5.Power.setVibration(255); delay(70); M5.Power.setVibration(0);  // ack
            return;
        }
    } else if (permissionPending) {
        permissionPending = false;                // window expired → revert buttons to normal
    }

    if (currentScreen == SCR_MEMO) {
        // ── Memo screen: Yellow = record locally, Blue = replay/stop ─────────
        if (M5.BtnA.wasPressed() && !memoRecording && !memoPlaying) {
            micOn();
            memoRecording = true;
            _memoSamples  = 0;
            micLevel      = 0.0f;
        }
        if (M5.BtnA.wasReleased() && memoRecording) {
            memoRecording = false;
            micLevel      = 0.0f;
            M5.Mic.end();
            speakerOn();
        }
        if (M5.BtnB.wasPressed()) {
            if (memoPlaying) {
                M5.Speaker.stop();
                memoPlaying = false;
            } else if (!memoRecording && _memoSamples > 0) {
                memoVolIdx = 2;  // reset to default (180) each new playback
                M5.Speaker.setVolume(MEMO_VOL_STEPS[memoVolIdx]);
                M5.Speaker.playRaw(_memoBuf, _memoSamples, AUDIO_SR, false);
                memoPlaying    = true;
                memoPlayStartMs = millis();
            }
        }
    } else {
        // ── Other screens: Yellow = STT record, Blue = inject transcript ──────
        if (M5.BtnA.wasPressed() && wsConnected && !recording && !transcribing) {
            micOn();
            recording     = true;
            transcribing  = false;
            audioQueued   = 0;
            micLevel      = 0.0f;
            recordStartMs = millis();
            lastTranscript = "";
            injected       = false;
            ws.sendTXT("CMD:RECORD_START");
        }
        if (M5.BtnA.wasReleased() && recording) {
            stopAndSendAudio();
        }
        if (M5.BtnB.wasPressed()) {
            bool valid = lastTranscript.length() > 0 && !lastTranscript.startsWith("(");
            if (valid && !injected) {
                ws.sendTXT("CMD:INJECT");   // 1st Blue press: paste into focused input
                injected = true;
            } else if (valid && injected) {
                ws.sendTXT("CMD:ENTER");     // 2nd Blue press: submit (Enter)
                lastTranscript = "";
                injected = false;
            } else {
                lastTranscript = "";          // error/empty transcript — just dismiss
                injected = false;
            }
        }
    }
}

// ── Touch swipe / tap — navigate screens + volume control ─────────────────────
static int16_t _touchStartX = 0, _touchStartY = 0;
static int16_t _touchCurrX  = 0, _touchCurrY  = 0;

void handleTouch() {
    auto t = M5.Touch.getDetail();
    if (t.wasPressed() || t.isPressed()) _lastActivityMs = millis();
    if (t.wasPressed()) {
        if (_displayAsleep) {
            wakeDisplay();
            _touchStartX = _touchCurrX = t.x;
            _touchStartY = _touchCurrY = t.y;
            return;  // first touch just wakes the screen
        }
        _touchStartX = _touchCurrX = t.x;
        _touchStartY = _touchCurrY = t.y;
    }
    if (t.isPressed()) {          // track position throughout drag
        _touchCurrX = t.x;
        _touchCurrY = t.y;
    }
    if (!t.wasReleased()) return;

    int16_t dx = _touchCurrX - _touchStartX;
    int16_t dy = _touchCurrY - _touchStartY;

    // Tap (finger barely moved): volume control during memo playback
    if (abs(dx) < 35 && abs(dy) < 35) {
        if (currentScreen == SCR_MEMO && memoPlaying) {
            bool volUp = (_touchCurrX >= M5.Display.width() / 2);
            if (volUp) {
                memoVolIdx = (memoVolIdx + 1) % MEMO_VOL_COUNT;
            } else {
                memoVolIdx = (memoVolIdx + MEMO_VOL_COUNT - 1) % MEMO_VOL_COUNT;
            }
            M5.Speaker.setVolume(MEMO_VOL_STEPS[memoVolIdx]);
        }
        return;
    }

    // Swipe left/right → navigate
    if (abs(dx) > 40 && abs(dx) > abs(dy)) {
        // Dismiss a lingering transcript so it can't block screen navigation
        if (lastTranscript.length() > 0 && !recording && !transcribing) {
            lastTranscript = "";
            injected = false;
        }
        Screen prev = currentScreen;
        if (dx < 0) {  // swipe left → advance
            currentScreen = (Screen)((int(currentScreen) + 1) % 3);
        } else {       // swipe right → back
            currentScreen = (Screen)((int(currentScreen) + 2) % 3);
        }
        // Stop memo playback when navigating away
        if (prev == SCR_MEMO && currentScreen != SCR_MEMO && memoPlaying) {
            M5.Speaker.stop();
            memoPlaying = false;
        }
        // Stop memo recording when navigating away
        if (prev == SCR_MEMO && currentScreen != SCR_MEMO && memoRecording) {
            memoRecording = false;
            micLevel = 0.0f;
            M5.Mic.end();
            speakerOn();
        }
    }
}

// ── Splash screen ─────────────────────────────────────────────────────────────
void showConnecting(const char* msg) {
    int cx = M5.Display.width()/2, cy = M5.Display.height()/2;
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextDatum(MC_DATUM);
    M5.Display.setFont(&fonts::Font6);
    M5.Display.setTextColor(M5.Display.color565(100,180,255));
    M5.Display.drawString("Claude", cx, cy-36);
    M5.Display.setFont(&fonts::Font4);
    M5.Display.setTextColor(M5.Display.color565(140,140,140));
    M5.Display.drawString(msg, cx, cy+24);
    M5.Display.setTextDatum(TL_DATUM);
}

// ── Attention banner (permission prompt / notification) ─────────────────────────
// showAllow = true when a permission prompt is awaiting approval → adds "Blue = Allow".
static void drawNotifyBanner(M5Canvas& cv, const String& msg, bool showAllow) {
    int W = cv.width(), H = cv.height(), cx = W / 2, cy = H / 2;
    int bw = (int)(W * 0.84f), bh = showAllow ? 128 : 104;
    int bx = cx - bw / 2, by = cy - bh / 2;
    uint16_t amber = cv.color565(255, 185, 30);

    cv.fillRoundRect(bx, by, bw, bh, 12, cv.color565(45, 33, 8));
    cv.drawRoundRect(bx, by, bw, bh, 12, amber);
    cv.drawRoundRect(bx + 1, by + 1, bw - 2, bh - 2, 11, amber);

    // warning triangle with "!"
    int tx = cx, ty = by + 22, ts = 12;
    cv.fillTriangle(tx, ty - ts, tx - ts, ty + ts, tx + ts, ty + ts, amber);
    cv.setTextDatum(MC_DATUM);
    cv.setTextColor(cv.color565(45, 33, 8));
    cv.setFont(&fonts::Font2);
    cv.drawString("!", tx, ty + 2);

    cv.setTextColor(amber);
    cv.setFont(&fonts::Font4);
    cv.drawString("Claude needs you", cx, by + 52);

    // message, truncated with ellipsis to fit the band
    cv.setFont(&fonts::Font2);
    cv.setTextColor(TFT_WHITE);
    String t = msg;
    bool trunc = false;
    while (t.length() > 1 &&
           cv.textWidth((trunc ? t + "..." : t).c_str()) > bw - 24) {
        t.remove(t.length() - 1);
        trunc = true;
    }
    if (trunc) t += "...";
    cv.drawString(t, cx, by + 78);

    // Allow / Deny hints (while a permission prompt is awaiting a decision)
    if (showAllow) {
        cv.setFont(&fonts::Font2);
        cv.setTextDatum(MR_DATUM);
        cv.setTextColor(cv.color565(60, 210, 60));
        cv.drawString("Blue=Allow", cx - 10, by + 108);
        cv.setTextDatum(ML_DATUM);
        cv.setTextColor(cv.color565(235, 80, 80));
        cv.drawString("Yellow=Deny", cx + 10, by + 108);
    }
    cv.setTextDatum(TL_DATUM);
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    auto cfg = M5.config();
    cfg.fallback_board = m5::board_t::board_M5StopWatch;
    M5.begin(cfg);

    // Microphone config stored now; begin() called on demand just before recording
    // so it doesn't lock the shared I2S clock pins (MCK=18,BCK=17,WS=15) away
    // from the speaker (same pins, different I2S port).
    auto micCfg = M5.Mic.config();
    micCfg.sample_rate   = AUDIO_SR;
    micCfg.stereo        = false;
    micCfg.magnification = 4;    // hardware PGA does the heavy lifting; software just fine-trims
    M5.Mic.config(micCfg);
    // DO NOT call M5.Mic.begin() here — speaker gets I2S pins by default.

    speakerOn();

    // Vibration motor boot test — 3 short pulses so you know the motor works
    for (int i = 0; i < 3; i++) {
        M5.Power.setVibration(255);
        delay(120);
        M5.Power.setVibration(0);
        delay(120);
    }

    M5.Display.setRotation(0);
    M5.Display.setBrightness(DISPLAY_BRIGHTNESS);

    M5.Display.fillScreen(TFT_RED);   delay(300);
    M5.Display.fillScreen(TFT_GREEN); delay(300);
    M5.Display.fillScreen(TFT_BLUE);  delay(300);
    M5.Display.fillScreen(TFT_BLACK);

    canvas.createSprite(M5.Display.width(), M5.Display.height());

    showConnecting("Connecting to WiFi...");
    WiFi.setAutoReconnect(true);   // re-join automatically if the AP drops us
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) delay(500);

    if (WiFi.status() == WL_CONNECTED) {
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);  // light modem sleep — stable on MIT enterprise WiFi
        configTzTime(POSIX_TZ, "ntp.mit.edu", "pool.ntp.org");
        showConnecting("Syncing time...");
        struct tm _t; int _ntpTry = 0;
        while (!getLocalTime(&_t) && _ntpTry++ < 20) delay(500);

        showConnecting("Connecting to bridge...");
        ws.begin(BRIDGE_IP, BRIDGE_PORT, "/");
        ws.onEvent(onWsEvent);
        ws.setReconnectInterval(5000);
    } else {
        showConnecting("WiFi failed — check config.h");
        delay(3000);
    }

    // Allocate voice memo buffer in PSRAM (15s × 16kHz × 2 bytes = 480KB)
    _memoBuf = (int16_t*)ps_malloc(MEMO_MAX_SAMPLES * sizeof(int16_t));

    sessionStartMs  = millis();
    _lastActivityMs = millis();
}

// ── Loop ─────────────────────────────────────────────────────────────────────
void loop() {
    M5.update();
    ws.loop();

    handleButtons();
    handleTouch();

    unsigned long now = millis();

    // ── Memo recording: capture to PSRAM buffer ───────────────────────────────
    if (memoRecording && _memoBuf != nullptr) {
        static int16_t _memoChunk[AUDIO_CHUNK];
        if (M5.Mic.record(_memoChunk, AUDIO_CHUNK, AUDIO_SR)) {
            if (_memoSamples + AUDIO_CHUNK <= MEMO_MAX_SAMPLES) {
                memcpy(_memoBuf + _memoSamples, _memoChunk, AUDIO_CHUNK * sizeof(int16_t));
                _memoSamples += AUDIO_CHUNK;
            } else {
                memoRecording = false;  // buffer full — auto-stop
                micLevel = 0.0f;
                M5.Mic.end();
                speakerOn();
            }
            float sum = 0;
            for (int i = 0; i < (int)AUDIO_CHUNK; i++) sum += abs(_memoChunk[i]);
            micLevel = 0.7f * micLevel + 0.3f * (sum / AUDIO_CHUNK / 16384.0f);
        }
    }

    // ── STT streaming: one DMA chunk per loop iteration → bridge ─────────────
    if (recording) {
        static int16_t _micChunk[AUDIO_CHUNK];

        if (M5.Mic.record(_micChunk, AUDIO_CHUNK, AUDIO_SR)) {
            if (wsConnected) {
                ws.sendBIN((uint8_t*)_micChunk, AUDIO_CHUNK * sizeof(int16_t));
                ws.loop();
            }
            float sum = 0;
            for (int i = 0; i < (int)AUDIO_CHUNK; i++) sum += abs(_micChunk[i]);
            micLevel = 0.7f * micLevel + 0.3f * (sum / AUDIO_CHUNK / 16384.0f);
            audioQueued += AUDIO_CHUNK;
        }

        if (now - recordStartMs > MAX_RECORD_MS) stopAndSendAudio();
    }

    // ── Memo playback: track completion ──────────────────────────────────────
    if (memoPlaying && !M5.Speaker.isPlaying()) {
        memoPlaying = false;
    }

    // Done-chime + vibration: fires on thinking→waiting transition.
    // 3-second cooldown guards against rapid re-thinking firing it twice.
    static unsigned long lastChimeMs = 0;
    if (chimePending && (millis() - lastChimeMs > 3000)) {
        chimePending = false;
        lastChimeMs = millis();
        M5.Speaker.setVolume(CHIME_VOLUME);      // quiet done-chime
        M5.Power.setVibration(255);
        M5.Speaker.tone(1047, 100); delay(130);  // C6
        M5.Speaker.tone(1319, 100); delay(130);  // E6
        M5.Speaker.tone(1568, 200); delay(250);  // G6
        M5.Power.setVibration(0);
        M5.Speaker.setVolume(255);               // restore for memo/alert playback
    } else if (chimePending) {
        chimePending = false;  // discard if within cooldown
    }

    // Attention alert (permission prompt / notification) — distinct from the
    // done-chime so the two are easy to tell apart. Wakes the screen.
    if (notifyPending) {
        notifyPending = false;
        notifyShownMs = millis();
        _lastActivityMs = millis();
        wakeDisplay();
        // urgent two-pulse "ding-dong" (high → low) with vibration
        M5.Speaker.setVolume(CHIME_VOLUME);
        for (int i = 0; i < 2; i++) {
            M5.Power.setVibration(255);
            M5.Speaker.tone(1760, 130); delay(150);  // A6
            M5.Speaker.tone(1175, 130); delay(150);  // D6
            M5.Power.setVibration(0);
            delay(90);
        }
        M5.Speaker.setVolume(255);               // restore for memo playback
    }

    // Heartbeat
    if (now - lastHeartbeatMs > HEARTBEAT_INTERVAL_MS) {
        sendHeartbeat();
        lastHeartbeatMs = now;
    }

    // WiFi watchdog: if the association dropped (APs deauth idle clients), force a
    // re-join. The WebSocket then re-establishes itself via setReconnectInterval.
    static unsigned long lastWifiCheck = 0;
    if (now - lastWifiCheck > 10000) {
        lastWifiCheck = now;
        if (WiFi.status() != WL_CONNECTED) {
            WiFi.reconnect();
        }
    }

    // Keep the screen awake (and wake it if asleep) while Claude is actively
    // working or an audio op is in flight — so the running animation stays visible.
    bool permActive = permissionPending && (now - permissionMs < PERMISSION_WINDOW_MS);

    // When Claude starts working, wake the screen once and jump to the home spinner.
    // Then the normal 5-min sleep applies even if Claude is still thinking.
    if (wakeForThink) {
        wakeForThink = false;
        currentScreen = SCR_STATUS;
        _lastActivityMs = now;
        wakeDisplay();
    }

    // Keep the screen ON the whole time Claude is working, plus during audio ops /
    // pending permission. (Normal 5-min idle sleep applies only when not working.)
    bool keepAwake = (claudeStatus == "thinking")
                   || recording || transcribing || memoRecording || memoPlaying || permActive;
    if (keepAwake) {
        _lastActivityMs = now;
        wakeDisplay();
    }

    // Display auto-sleep: turn off after DISPLAY_SLEEP_MS of no touch/button.
    // Wake is handled in handleTouch / handleButtons on first interaction.
    if (!_displayAsleep && (now - _lastActivityMs > DISPLAY_SLEEP_MS)) {
        sleepDisplay();
    }
    if (_displayAsleep) {
        // Screen off: idle the loop so the SoC can drop into lighter power states
        // (WiFi stays associated via modem-sleep; ws.loop already ran this tick).
        delay(60);
        return;
    }

    // animFrame at 20Hz wall-clock (independent of draw rate)
    animFrame = now / 50;

    bool notifyActive = permActive || (notifyMsg.length() && (now - notifyShownMs < 5000));
    bool needsAnim = recording || transcribing || memoRecording || memoPlaying
                   || (claudeStatus == "thinking")
                   || (claudeStatus == "waiting")
                   || notifyActive;
    unsigned long drawInterval = needsAnim ? 50 : 1000;

    if (now - lastDrawMs > drawInterval) {
        lastDrawMs = now;
        uint32_t sessionSec = (now - sessionStartMs) / 1000;

        // Voice STT screen takes over when recording/transcribing/transcript active
        if (recording || transcribing || lastTranscript.length() > 0) {
            renderVoiceScreen(canvas, recording, transcribing,
                              lastTranscript, battPct, animFrame, micLevel, injected);
        } else {
            switch (currentScreen) {
                case SCR_STATUS:
                    renderStatusScreen(canvas, wsConnected, claudeStatus,
                                       battPct, lastTranscript, animFrame,
                                       weatherCode, weatherTemp,
                                       recording, transcribing, micLevel);
                    break;
                case SCR_TOKENS:
                    renderUsageScreen(canvas,
                                      usageSessPct, usageSessResetMin,
                                      usageWeekAllPct, usageWeekSonPct,
                                      usageWeekReset, currentModel, sessionSec);
                    break;
                case SCR_MEMO: {
                    float playFrac = 0.0f;
                    if (memoPlaying && _memoSamples > 0) {
                        float totalMs = _memoSamples * 1000.0f / AUDIO_SR;
                        playFrac = min(1.0f, (millis() - memoPlayStartMs) / totalMs);
                    }
                    renderMemoScreen(canvas, memoRecording, memoPlaying,
                                     _memoSamples, animFrame, micLevel, playFrac,
                                     memoVolIdx, MEMO_VOL_COUNT);
                    break;
                }
            }
        }
        // Attention banner overlays any screen after a NOTIFY (permission → Blue=Allow)
        if (notifyActive) {
            drawNotifyBanner(canvas, notifyMsg, permActive);
        }
        canvas.pushSprite(0, 0);
    }
}
