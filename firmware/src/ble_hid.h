#pragma once
// ── iPhone remote: BLE HID mouse + keyboard + consumer keys ──────────────────
// The watch shows up on the iPhone as a Bluetooth keyboard/mouse ("StopWatch
// Remote"). iOS draws a pointer for it when AssistiveTouch is on. The PC app
// (remote/iphone_remote.py) drives it with one-line text commands over USB
// serial (COM7); the bridge can send the same lines over the WebSocket as "HID:...".
//
// BLE is OFF at boot so the watch keeps its usual WiFi behaviour. "ble 1" turns
// it on (WiFi drops to modem-sleep, which ESP32 coexistence requires), "ble 0"
// turns it off again and restores WIFI_PS_NONE.
//
// Commands (one per line):
//   ble 1|0          radio on/off            ?            → "HID ble=1 conn=1"
//   m dx dy [w] [p]  relative move/scroll    b mask       buttons (1=L 2=R 4=M)
//   H                pointer to top-left     g x y        top-left, then x,y counts
//   p step ms        pacing used by g/H      c mask       click (press+release)
//   k mod k1..k6     raw keyboard report     K mod key    tap one key
//   t text           type ASCII text         u usage      tap a consumer key
//   forget           delete all bonds

#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <esp_wifi.h>

volatile bool hidEnabled   = false;
volatile bool hidConnected = false;

static NimBLEServer*         _hidServer = nullptr;
static NimBLEHIDDevice*      _hid       = nullptr;
static NimBLECharacteristic* _kbIn      = nullptr;
static NimBLECharacteristic* _msIn      = nullptr;
static NimBLECharacteristic* _ccIn      = nullptr;
static QueueHandle_t         _hidQueue  = nullptr;   // lines from the WebSocket
static uint8_t               _btnMask   = 0;
static int                   _paceStep  = 16;        // counts per report for g/H
static int                   _paceMs    = 8;         // ms between paced reports

static const uint8_t _hidReportMap[] = {
    // Keyboard, report ID 1
    0x05,0x01, 0x09,0x06, 0xA1,0x01, 0x85,0x01,
    0x05,0x07, 0x19,0xE0, 0x29,0xE7, 0x15,0x00, 0x25,0x01, 0x75,0x01, 0x95,0x08, 0x81,0x02,
    0x95,0x01, 0x75,0x08, 0x81,0x01,
    0x95,0x05, 0x75,0x01, 0x05,0x08, 0x19,0x01, 0x29,0x05, 0x91,0x02,
    0x95,0x01, 0x75,0x03, 0x91,0x01,
    0x95,0x06, 0x75,0x08, 0x15,0x00, 0x25,0x73, 0x05,0x07, 0x19,0x00, 0x29,0x73, 0x81,0x00,
    0xC0,
    // Mouse, report ID 2: buttons, X, Y, wheel, AC pan
    0x05,0x01, 0x09,0x02, 0xA1,0x01, 0x85,0x02, 0x09,0x01, 0xA1,0x00,
    0x05,0x09, 0x19,0x01, 0x29,0x05, 0x15,0x00, 0x25,0x01, 0x95,0x05, 0x75,0x01, 0x81,0x02,
    0x95,0x01, 0x75,0x03, 0x81,0x01,
    0x05,0x01, 0x09,0x30, 0x09,0x31, 0x09,0x38, 0x15,0x81, 0x25,0x7F, 0x75,0x08, 0x95,0x03, 0x81,0x06,
    0x05,0x0C, 0x0A,0x38,0x02, 0x15,0x81, 0x25,0x7F, 0x75,0x08, 0x95,0x01, 0x81,0x06,
    0xC0, 0xC0,
    // Consumer control, report ID 3: one 16-bit usage
    0x05,0x0C, 0x09,0x01, 0xA1,0x01, 0x85,0x03,
    0x15,0x00, 0x26,0xFF,0x03, 0x19,0x00, 0x2A,0xFF,0x03, 0x75,0x10, 0x95,0x01, 0x81,0x00,
    0xC0,
};

class _HidServerCB : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
        hidConnected = true;
    }
    void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
        hidConnected = false;
        if (hidEnabled) NimBLEDevice::startAdvertising();   // let the iPhone reconnect
    }
};

static bool _notify(NimBLECharacteristic* c, const uint8_t* d, size_t n) {
    if (!hidConnected || !c) return false;
    for (int i = 0; i < 25; i++) {          // retry while the TX buffers are full
        if (c->notify(d, n)) return true;
        delay(2);
    }
    return false;
}

static void _mouseReport(int dx, int dy, int wheel = 0, int pan = 0) {
    uint8_t r[5] = { _btnMask, (uint8_t)(int8_t)dx, (uint8_t)(int8_t)dy,
                     (uint8_t)(int8_t)wheel, (uint8_t)(int8_t)pan };
    _notify(_msIn, r, sizeof(r));
}

static int _clamp127(int v) { return v > 127 ? 127 : (v < -127 ? -127 : v); }

// Relative move, split into ±127 chunks. paced → fixed step size + interval, so
// iOS sees a constant pointer speed (constant acceleration → linear calibration).
static void _move(long dx, long dy, int wheel, int pan, bool paced) {
    int lim = paced ? _paceStep : 127;
    do {
        int sx = (int)(dx > lim ? lim : (dx < -lim ? -lim : dx));
        int sy = (int)(dy > lim ? lim : (dy < -lim ? -lim : dy));
        _mouseReport(sx, sy, _clamp127(wheel), _clamp127(pan));
        wheel = pan = 0;
        dx -= sx; dy -= sy;
        if (paced) delay(_paceMs);
    } while (dx != 0 || dy != 0);
}

static void _home() {
    // Far more than any screen in counts; the pointer pins to the top-left corner.
    for (int i = 0; i < 40; i++) { _mouseReport(-127, -127); delay(4); }
    delay(30);
}

static void _kbReport(uint8_t mod, const uint8_t keys[6]) {
    uint8_t r[8] = { mod, 0, keys[0], keys[1], keys[2], keys[3], keys[4], keys[5] };
    _notify(_kbIn, r, sizeof(r));
}

static void _tapKey(uint8_t mod, uint8_t key) {
    uint8_t k[6] = { key, 0, 0, 0, 0, 0 };
    uint8_t z[6] = { 0 };
    _kbReport(mod, k); delay(8);
    _kbReport(0, z);   delay(8);
}

static void _tapConsumer(uint16_t usage) {
    uint8_t r[2] = { (uint8_t)(usage & 0xFF), (uint8_t)(usage >> 8) };
    uint8_t z[2] = { 0, 0 };
    _notify(_ccIn, r, 2); delay(30);
    _notify(_ccIn, z, 2); delay(10);
}

// US layout: ASCII → (HID usage, needs shift). Returns false if unmappable.
static bool _asciiToKey(char c, uint8_t& key, bool& shift) {
    static const char* sh1 = "!@#$%^&*()";
    shift = false;
    if (c >= 'a' && c <= 'z') { key = 0x04 + (c - 'a'); return true; }
    if (c >= 'A' && c <= 'Z') { key = 0x04 + (c - 'A'); shift = true; return true; }
    if (c >= '1' && c <= '9') { key = 0x1E + (c - '1'); return true; }
    if (c == '0') { key = 0x27; return true; }
    for (int i = 0; i < 10; i++) if (c == sh1[i]) { key = (i == 9) ? 0x27 : 0x1E + i; shift = true; return true; }
    struct M { char a, b; uint8_t k; };   // unshifted, shifted, usage
    static const M m[] = {
        {'-','_',0x2D}, {'=','+',0x2E}, {'[','{',0x2F}, {']','}',0x30}, {'\\','|',0x31},
        {';',':',0x33}, {'\'','"',0x34}, {'`','~',0x35}, {',','<',0x36}, {'.','>',0x37}, {'/','?',0x38},
    };
    for (auto& e : m) {
        if (c == e.a) { key = e.k; return true; }
        if (c == e.b) { key = e.k; shift = true; return true; }
    }
    if (c == ' ')  { key = 0x2C; return true; }
    if (c == '\n') { key = 0x28; return true; }
    if (c == '\t') { key = 0x2B; return true; }
    return false;
}

static void _typeText(const String& s) {
    for (size_t i = 0; i < s.length(); i++) {
        char c = s[i];
        if (c == '\\' && i + 1 < s.length()) {          // \n and \t escapes (one-line protocol)
            char e = s[i + 1];
            if (e == 'n') { c = '\n'; i++; }
            else if (e == 't') { c = '\t'; i++; }
            else if (e == '\\') { i++; }
        }
        uint8_t key; bool shift;
        if (_asciiToKey(c, key, shift)) _tapKey(shift ? 0x02 : 0x00, key);
    }
}

static void hidEnable() {
    if (hidEnabled) return;
    // Coexistence needs WiFi modem-sleep while BLE runs; restored by hidDisable().
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);

    NimBLEDevice::init("StopWatch Remote");
    NimBLEDevice::setSecurityAuth(true, false, true);          // bond, no MITM, LE secure
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);  // "Just Works" pairing

    _hidServer = NimBLEDevice::createServer();
    _hidServer->setCallbacks(new _HidServerCB());
    _hid = new NimBLEHIDDevice(_hidServer);
    _hid->setManufacturer("M5Stack");
    _hid->setPnp(0x02, 0x303A, 0x8211, 0x0100);
    _hid->setHidInfo(0x00, 0x01);
    _hid->setReportMap((uint8_t*)_hidReportMap, sizeof(_hidReportMap));
    _kbIn = _hid->getInputReport(1);
    _hid->getOutputReport(1);                                    // keyboard LEDs (unused)
    _msIn = _hid->getInputReport(2);
    _ccIn = _hid->getInputReport(3);
    _hid->setBatteryLevel(100);
    _hidServer->start();

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->setAppearance(0x03C0);                                  // generic HID
    adv->addServiceUUID(_hid->getHidService()->getUUID());
    adv->setName("StopWatch Remote");
    adv->enableScanResponse(true);
    adv->start();
    hidEnabled = true;
}

static void hidDisable() {
    if (!hidEnabled) return;
    hidEnabled = hidConnected = false;
    NimBLEDevice::deinit(true);
    _hidServer = nullptr; _hid = nullptr; _kbIn = _msIn = _ccIn = nullptr;
    _btnMask = 0;
    esp_wifi_set_ps(WIFI_PS_NONE);
}

static void hidSetBattery(int pct) {
    if (hidEnabled && _hid && pct >= 0 && pct <= 100) _hid->setBatteryLevel((uint8_t)pct, hidConnected);
}

static void _hidCommand(String line) {
    line.trim();
    if (line.length() == 0) return;
    char cmd = line[0];
    String rest = line.substring(1); rest.trim();
    long a[8] = { 0 }; int n = 0;
    auto parseNums = [&]() {
        const char* p = rest.c_str();
        while (*p && n < 8) {
            char* end;
            long v = strtol(p, &end, 0);
            if (end == p) break;
            a[n++] = v; p = end;
        }
    };

    if (line.startsWith("ble")) {
        String v = line.substring(3); v.trim();
        if (v == "1") hidEnable(); else if (v == "0") hidDisable();
        Serial.printf("HID ble=%d conn=%d\n", hidEnabled ? 1 : 0, hidConnected ? 1 : 0);
        return;
    }
    if (line == "forget") { if (hidEnabled) NimBLEDevice::deleteAllBonds(); Serial.println("HID forgot"); return; }
    if (cmd == '?') { Serial.printf("HID ble=%d conn=%d\n", hidEnabled ? 1 : 0, hidConnected ? 1 : 0); return; }
    if (!hidConnected) return;

    switch (cmd) {
        case 'm': parseNums(); _move(a[0], a[1], (int)a[2], (int)a[3], false); break;
        case 'b': parseNums(); _btnMask = (uint8_t)a[0]; _mouseReport(0, 0); break;
        case 'c': parseNums(); _btnMask = (uint8_t)a[0]; _mouseReport(0, 0); delay(40);
                  _btnMask = 0; _mouseReport(0, 0); break;
        case 'H': _home(); break;
        case 'g': parseNums(); _home(); _move(a[0], a[1], 0, 0, true); break;
        case 'p': parseNums(); if (a[0] > 0) _paceStep = (int)a[0]; if (a[1] > 0) _paceMs = (int)a[1]; break;
        case 'k': { parseNums(); uint8_t k[6] = { 0 };
                    for (int i = 0; i < 6 && i + 1 < n; i++) k[i] = (uint8_t)a[i + 1];
                    _kbReport((uint8_t)a[0], k); break; }
        case 'K': parseNums(); _tapKey((uint8_t)a[0], (uint8_t)a[1]); break;
        case 't': _typeText(line.substring(2)); break;     // keep leading spaces after "t "
        case 'u': parseNums(); _tapConsumer((uint16_t)a[0]); break;
    }
}

// Lines from the WebSocket ("HID:<line>") are queued so all HID work stays in one task.
static void hidEnqueue(const String& line) {
    if (!_hidQueue) return;
    char* copy = strdup(line.c_str());
    if (copy && xQueueSend(_hidQueue, &copy, 0) != pdTRUE) free(copy);
}

static void _hidTask(void*) {
    String buf;
    for (;;) {
        bool idle = true;
        while (Serial.available()) {
            idle = false;
            char c = (char)Serial.read();
            if (c == '\n') { _hidCommand(buf); buf = ""; }
            else if (c != '\r' && buf.length() < 1024) buf += c;
        }
        char* q;
        while (xQueueReceive(_hidQueue, &q, 0) == pdTRUE) {
            idle = false;
            _hidCommand(String(q));
            free(q);
        }
        if (idle) vTaskDelay(pdMS_TO_TICKS(2));
    }
}

static void hidBegin() {
    Serial.setRxBufferSize(4096);
    Serial.setTxTimeoutMs(20);   // never stall if no PC app is reading
    Serial.begin(115200);        // M5Unified leaves Serial unstarted (serial_baudrate = 0)
    _hidQueue = xQueueCreate(32, sizeof(char*));
    xTaskCreatePinnedToCore(_hidTask, "hid", 8192, nullptr, 2, nullptr, 0);
}
