#pragma once
#include <M5Unified.h>
#include <math.h>

// ── Weather icon primitives ───────────────────────────────────────────────────

static inline void _wSun(M5Canvas& cv, int cx, int cy) {
    uint16_t y = cv.color565(255, 200, 0);
    for (int i = 0; i < 8; i++) {
        float a = i * M_PI / 4.0f;
        cv.drawLine(cx+(int)(27*cosf(a)), cy+(int)(27*sinf(a)),
                    cx+(int)(36*cosf(a)), cy+(int)(36*sinf(a)), y);
    }
    cv.fillCircle(cx, cy, 21, y);
}

static inline void _wCloud(M5Canvas& cv, int cx, int cy, uint16_t col) {
    cv.fillCircle(cx-15, cy+5,  15, col);
    cv.fillCircle(cx+13, cy+1,  17, col);
    cv.fillCircle(cx-1,  cy-10, 13, col);
    cv.fillRect(cx-30, cy+5, 60, 17, col);
}

static inline void _wDrops(M5Canvas& cv, int cx, int cy) {
    uint16_t d = cv.color565(80, 160, 255);
    int xs[] = {cx-18, cx-6, cx+6, cx+18};
    for (int i = 0; i < 4; i++) {
        cv.fillCircle(xs[i],   cy,    4, d);
        cv.fillCircle(xs[i]-4, cy+12, 4, d);
    }
}

static inline void _wSnow(M5Canvas& cv, int cx, int cy) {
    uint16_t s = cv.color565(200, 230, 255);
    int xs[] = {cx-18, cx-6, cx+6, cx+18};
    for (int i = 0; i < 4; i++) {
        cv.fillCircle(xs[i], cy+2, 4, s);
        cv.drawFastHLine(xs[i]-6, cy+2, 12, s);
        cv.drawFastVLine(xs[i], cy-4, 12, s);
    }
}

static inline void drawWeatherIcon(M5Canvas& cv, int cx, int cy, int code) {
    uint16_t white = cv.color565(220, 220, 220);
    uint16_t gray  = cv.color565(160, 160, 180);
    if      (code == 0)                            { _wSun(cv, cx, cy); }
    else if (code <= 2)                            { _wSun(cv, cx-12, cy-8); _wCloud(cv, cx+6, cy+6, white); }
    else if (code <= 48)                           { _wCloud(cv, cx, cy-6, white); _wCloud(cv, cx+4, cy+10, gray); }
    else if (code <= 67 || (code>=80 && code<=82)) { _wCloud(cv, cx, cy-12, white); _wDrops(cv, cx, cy+18); }
    else if (code <= 77)                           { _wCloud(cv, cx, cy-12, white); _wSnow(cv, cx, cy+20); }
    else {
        _wCloud(cv, cx, cy-12, white);
        uint16_t bolt = cv.color565(255,240,0);
        cv.fillTriangle(cx-2,cy+6, cx+8,cy+6, cx+1,cy+18, bolt);
        cv.fillTriangle(cx-8,cy+16, cx+2,cy+16, cx-5,cy+28, bolt);
        _wDrops(cv, cx, cy+30);
    }
}

// ── Battery icon ─────────────────────────────────────────────────────────────

inline void drawBatteryIcon(M5Canvas& cv, int x, int y, int pct) {
    uint16_t c = (pct > 20) ? cv.color565(80,220,80) : cv.color565(220,60,60);
    cv.drawRect(x, y, 34, 16, TFT_WHITE);
    cv.fillRect(x+34, y+4, 5, 8, TFT_WHITE);
    cv.fillRect(x+1, y+1, max(1,(pct*32)/100), 14, c);
    cv.setFont(&fonts::Font2); cv.setTextSize(1);
    cv.setTextColor(cv.color565(200,200,200));
    cv.setTextDatum(MR_DATUM);
    cv.drawString(String(pct)+"%", x-4, y+8);
    cv.setTextDatum(TL_DATUM);
}

// ── Navigation dots ───────────────────────────────────────────────────────────

inline void drawDots(M5Canvas& cv, int cx, int H, int active) {
    int sp=24, y=H-18;
    for (int i=0; i<3; i++) {
        int x = cx+(i-1)*sp;
        if (i==active) cv.fillCircle(x, y, 6, TFT_WHITE);
        else            cv.drawCircle(x, y, 6, cv.color565(70,70,70));
    }
}

// ── Unified status screen (watch face + status overlay) ───────────────────────
// The watch face is ALWAYS drawn. Claude status and "No Bridge" are overlaid.

inline void renderStatusScreen(M5Canvas& cv, bool connected,
                                const String& status, int batt,
                                const String& lastTranscript,
                                uint32_t animFrame,
                                int weatherCode, int weatherTemp,
                                bool recording = false,
                                bool transcribing = false,
                                float micLevel = 0.0f,
                                const String& gptStatus = "idle") {
    int W = cv.width(), H = cv.height();
    int cx = W/2, cy = H/2;

    cv.fillScreen(TFT_BLACK);

    // ── Watch face (always shown) ─────────────────────────────────────────────
    struct tm t;
    if (getLocalTime(&t)) {
        char hbuf[3], mbuf[3];
        snprintf(hbuf, sizeof(hbuf), "%02d", t.tm_hour);
        snprintf(mbuf, sizeof(mbuf), "%02d", t.tm_min);

        // Left: large stacked hour / minute
        int lx = 118;
        cv.setFont(&fonts::Font7);
        cv.setTextSize(3);
        cv.setTextDatum(MC_DATUM);
        cv.setTextColor(TFT_WHITE);
        cv.drawString(hbuf, lx, 155);
        cv.drawFastHLine(lx-62, 225, 124, cv.color565(45,45,45));
        cv.drawString(mbuf, lx, 297);
        cv.setTextSize(1);

        // Battery % — top center of screen
        {
            uint16_t bc = (batt > 20) ? cv.color565(100, 230, 100)
                                       : cv.color565(255, 80,  80);
            char bbuf[8];
            snprintf(bbuf, sizeof(bbuf), "%d%%", batt);
            cv.setFont(&fonts::Font4);
            cv.setTextDatum(MC_DATUM);
            cv.setTextColor(bc);
            cv.drawString(bbuf, cx, 34);
        }

        // Right column: weather → date → temp
        int rx = 338;

        // Weather icon
        if (weatherCode >= 0) drawWeatherIcon(cv, rx - 5, 148, weatherCode);

        // Day of week (small dim label above date)
        static const char* _DOW[] = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
        cv.setFont(&fonts::Font2);
        cv.setTextDatum(MC_DATUM);
        cv.setTextColor(cv.color565(100, 100, 120));
        cv.drawString(_DOW[t.tm_wday], rx, 210);

        // Date MM/DD — Font6 for bigger, more readable display
        char dbuf[8];
        snprintf(dbuf, sizeof(dbuf), "%02d-%02d", t.tm_mon+1, t.tm_mday);
        cv.setFont(&fonts::Font6);
        cv.setTextColor(cv.color565(230, 230, 240));
        cv.drawString(dbuf, rx, 248);

        // Temperature — Celsius, Font6, color-coded by warmth
        if (weatherCode >= 0) {
            uint16_t tempCol;
            if      (weatherTemp <= 0)  tempCol = cv.color565(130, 200, 255); // freezing
            else if (weatherTemp <= 10) tempCol = cv.color565(160, 220, 255); // cool
            else if (weatherTemp <= 20) tempCol = cv.color565(100, 230, 160); // comfortable
            else if (weatherTemp <= 28) tempCol = cv.color565(255, 200, 80);  // warm
            else if (weatherTemp <= 35) tempCol = cv.color565(255, 130, 50);  // hot
            else                        tempCol = cv.color565(255, 70,  50);  // very hot
            // Font6 is ASCII-only — draw degree as a small circle, then number and C
            char tbuf[6];
            snprintf(tbuf, sizeof(tbuf), "%d", weatherTemp);
            cv.setFont(&fonts::Font6);
            cv.setTextColor(tempCol);
            cv.setTextDatum(TL_DATUM);
            int32_t tw = cv.textWidth(tbuf);
            int32_t tx = rx - (tw + 14) / 2;  // left edge so whole "18°C" is centred
            cv.drawString(tbuf, tx, 303);
            cv.drawCircle(tx + tw + 5, 306, 4, tempCol);   // ° circle
            cv.drawCircle(tx + tw + 5, 306, 3, tempCol);
            cv.drawString("C", tx + tw + 12, 303);
            cv.setTextDatum(MC_DATUM);
        }
    }

    // ── Bottom overlay — priority: transcript > recording > transcribing > Claude status ──
    cv.setTextDatum(MC_DATUM);

    if (lastTranscript.length() > 0) {
        String prev = lastTranscript.substring(0, min(22, (int)lastTranscript.length()));
        if ((int)lastTranscript.length() > 22) prev += "..";
        cv.setFont(&fonts::Font2);
        cv.setTextColor(cv.color565(200, 180, 60));
        cv.drawString("\"" + prev + "\"", cx, H - 70);
        cv.setTextColor(cv.color565(60, 180, 60));
        cv.drawString("Blue = Send to Claude", cx, H - 48);

    } else if (recording) {
        bool blink = (animFrame / 5) % 2;
        cv.fillCircle(cx - 50, H - 48, blink ? 7 : 5,
                      blink ? cv.color565(255, 50, 50) : cv.color565(120, 20, 20));
        float base = 0.3f + 0.7f * micLevel;
        for (int i = 0; i < 5; i++) {
            float w = base * (0.4f + 0.6f * sinf(animFrame * 0.25f + i * 1.1f));
            int bh = 4 + (int)(10.0f * w);
            cv.fillRect(cx - 30 + i * 8, H - 48 - bh/2, 5, bh, cv.color565(200, 50, 50));
        }
        cv.setFont(&fonts::Font2);
        cv.setTextDatum(ML_DATUM);
        cv.setTextColor(cv.color565(220, 60, 60));
        cv.drawString("REC  Release Yellow", cx + 16, H - 48);
        cv.setTextDatum(MC_DATUM);

    } else if (transcribing) {
        int sa = (int)(animFrame * 4) % 360;
        cv.drawArc(cx - 46, H - 48, 10, 6, sa, (sa + 120) % 360, cv.color565(60, 160, 255));
        cv.setFont(&fonts::Font2);
        cv.setTextDatum(ML_DATUM);
        cv.setTextColor(cv.color565(60, 160, 255));
        cv.drawString("Transcribing...", cx - 32, H - 48);
        cv.setTextDatum(MC_DATUM);

    } else if (!connected) {
        cv.setFont(&fonts::Font2);
        cv.setTextColor(cv.color565(220, 60, 60));
        cv.drawString("No Bridge \x97 start bridge.py", cx, H - 48);

    } else if (status == "thinking" || gptStatus == "thinking") {
        // Both can run at once: Claude = cyan OUTER ring, ChatGPT = red INNER ring.
        bool claudeT = (status == "thinking");
        bool gptT    = (gptStatus == "thinking");
        int a = (int)(animFrame * 3) % 360;
        if (claudeT) {
            cv.drawArc(cx, cy, 222, 210, a,            (a + 100) % 360, cv.color565(0, 220, 255));
            cv.drawArc(cx, cy, 222, 210, (a+180)%360,  (a + 280) % 360, cv.color565(0, 80, 140));
        }
        if (gptT) {
            cv.drawArc(cx, cy, 198, 186, a,            (a + 100) % 360, cv.color565(255, 60, 60));
            cv.drawArc(cx, cy, 198, 186, (a+180)%360,  (a + 280) % 360, cv.color565(140, 20, 20));
        }
        cv.setFont(&fonts::Font2);
        bool blink = (animFrame / 10) % 2;
        if (claudeT && gptT) {
            cv.setTextColor(blink ? cv.color565(0, 200, 240) : cv.color565(0, 90, 130));
            cv.setTextDatum(MR_DATUM);
            cv.drawString("Claude", cx - 6, H - 48);
            cv.setTextColor(blink ? cv.color565(255, 90, 90) : cv.color565(150, 40, 40));
            cv.setTextDatum(ML_DATUM);
            cv.drawString("+ ChatGPT", cx + 6, H - 48);
            cv.setTextDatum(MC_DATUM);
        } else if (gptT) {
            cv.setTextColor(blink ? cv.color565(255, 90, 90) : cv.color565(150, 40, 40));
            cv.drawString("ChatGPT thinking...", cx, H - 48);
        } else {
            cv.setTextColor(blink ? cv.color565(0, 200, 240) : cv.color565(0, 100, 140));
            cv.drawString("Claude thinking...", cx, H - 48);
        }

    } else if (status == "waiting") {
        bool pulse = (animFrame / 10) % 2;
        cv.fillCircle(cx - 52, H - 48, 6, pulse ? cv.color565(50, 210, 50) : cv.color565(20, 70, 20));
        cv.setFont(&fonts::Font2);
        cv.setTextDatum(ML_DATUM);
        cv.setTextColor(cv.color565(50, 210, 50));
        cv.drawString("Ready \x97 hold Yellow", cx - 40, H - 48);
        cv.setTextDatum(MC_DATUM);

    } else {
        cv.setFont(&fonts::Font2);
        cv.setTextColor(cv.color565(50, 50, 50));
        cv.drawString("Claude idle", cx, H - 48);
    }

    // ── Nav dots: 3 screens, left = active (index 0) ────────────────────
    int nsp = 24, ndy = H - 18;
    for (int i = 0; i < 3; i++) {
        int nx = cx + (i-1) * nsp;
        if (i == 0) cv.fillCircle(nx, ndy, 6, TFT_WHITE);
        else        cv.drawCircle(nx, ndy, 6, cv.color565(70, 70, 70));
    }
    cv.setTextDatum(TL_DATUM);
}
