#pragma once
#include <M5Unified.h>
#include <math.h>

// Claude starburst icon (8-ray asterisk)
static inline void _tkDrawStar(M5Canvas& cv, int ix, int iy, int r, uint16_t col) {
    for (int a = 0; a < 8; a++) {
        float ang = a * M_PI / 4.0f;
        int x0 = ix + (int)(r * 0.42f * cosf(ang));
        int y0 = iy + (int)(r * 0.42f * sinf(ang));
        int x1 = ix + (int)(r * cosf(ang));
        int y1 = iy + (int)(r * sinf(ang));
        cv.drawLine(x0, y0, x1, y1, col);
        cv.drawLine(x0+1, y0, x1+1, y1, col);
    }
    cv.fillCircle(ix, iy, (int)(r * 0.38f), col);
}

// One labeled bar row (label left, value right, progress bar between).
// baseCol sets the row's identity colour; bars near their limit flip to red.
static inline void _tkBar(M5Canvas& cv, int bL, int bR, int y, int barH,
                           const char* label, const char* valStr, float frac,
                           uint16_t baseCol) {
    frac = frac < 0 ? 0 : frac > 1 ? 1 : frac;
    uint16_t bc = (frac >= 0.90f) ? cv.color565(255, 70, 70) : baseCol;

    cv.setFont(&fonts::Font2);
    cv.setTextColor(cv.color565(150, 150, 175));
    cv.setTextDatum(ML_DATUM);
    cv.drawString(label, bL, y + barH / 2);

    cv.setTextColor(bc);
    cv.setTextDatum(MR_DATUM);
    cv.drawString(valStr, bR, y + barH / 2);

    int labW = 58, valW = 44;
    int barX = bL + labW, barEnd = bR - valW;
    int barW = barEnd - barX;
    cv.fillRoundRect(barX, y, barW, barH, 5, cv.color565(30, 30, 46));
    if (frac > 0.0f) {
        int fw = max(barH, (int)(barW * frac));
        cv.fillRoundRect(barX, y, fw, barH, 5, bc);
    }
}

// Small grey caption, centered
static inline void _tkCaption(M5Canvas& cv, int cx, int y, const char* s) {
    cv.setFont(&fonts::Font2);
    cv.setTextDatum(MC_DATUM);
    cv.setTextColor(cv.color565(95, 95, 120));
    cv.drawString(s, cx, y);
}

// One provider block: a coloured title + a 5h bar + a Week bar + a weekly-reset note.
// pct < 0 → "waiting for data". Returns nothing; draws at the given top y.
static inline void _tkProvider(M5Canvas& cv, int barL, int barR, int cx, int topY,
                               const char* name, uint16_t nameCol,
                               int sessPct, int weekPct, const String& weekReset,
                               uint16_t sessCol, uint16_t weekCol) {
    int barH = 16;
    cv.setFont(&fonts::Font2);
    cv.setTextDatum(ML_DATUM);
    cv.setTextColor(nameCol);
    cv.drawString(name, barL, topY);
    if (sessPct < 0) {
        cv.setTextColor(cv.color565(95, 95, 120));
        cv.setTextDatum(MR_DATUM);
        cv.drawString("waiting for data", barR, topY);
        cv.setTextDatum(ML_DATUM);
        return;
    }
    char buf[12];
    snprintf(buf, sizeof(buf), "%d%%", sessPct);
    _tkBar(cv, barL, barR, topY + 14, barH, "5h",   buf, sessPct / 100.0f, sessCol);
    snprintf(buf, sizeof(buf), "%d%%", weekPct);
    _tkBar(cv, barL, barR, topY + 38, barH, "Week", buf, weekPct / 100.0f, weekCol);
    if (weekReset.length() > 0) {
        char rbuf[40];
        snprintf(rbuf, sizeof(rbuf), "resets %s", weekReset.c_str());
        cv.setFont(&fonts::Font2);
        cv.setTextDatum(MR_DATUM);
        cv.setTextColor(cv.color565(90, 90, 115));
        cv.drawString(rbuf, barR, topY + 60);
        cv.setTextDatum(ML_DATUM);
    }
}

// ── Usage screen — Claude + ChatGPT rate limits, two stacked sections ───────────
inline void renderUsageScreen(M5Canvas& cv,
                               int sessPct, int sessResetMin,
                               int weekAllPct, int weekSonPct,
                               const String& weekReset,
                               int gptSessPct, int gptSessResetMin,
                               int gptWeekPct, const String& gptWeekReset,
                               const String& model,
                               uint32_t sessionSec) {
    (void)weekSonPct; (void)sessResetMin; (void)gptSessResetMin;
    (void)model; (void)sessionSec;
    int W = cv.width(), H = cv.height();
    int cx = W / 2, cy = H / 2;
    cv.fillScreen(TFT_BLACK);

    int halfDiv = (int)(W * 0.39f);
    int barL = cx - (int)(W * 0.37f);
    int barR = cx + (int)(W * 0.37f);

    // Header
    int iconX = cx - 44;
    _tkDrawStar(cv, iconX, cy - 118, 16, cv.color565(0, 150, 255));
    cv.setFont(&fonts::Font4);
    cv.setTextDatum(ML_DATUM);
    cv.setTextColor(TFT_WHITE);
    cv.drawString("Usage", iconX + 24, cy - 118);
    cv.drawFastHLine(cx - halfDiv, cy - 98, 2 * halfDiv, cv.color565(36, 36, 56));

    // Claude — cyan title, blue 5h / violet Week
    _tkProvider(cv, barL, barR, cx, cy - 80, "Claude", cv.color565(0, 180, 255),
                sessPct, weekAllPct, weekReset,
                cv.color565(0, 150, 255), cv.color565(175, 120, 255));

    // ChatGPT — red title, red 5h / amber Week
    _tkProvider(cv, barL, barR, cx, cy + 10, "ChatGPT", cv.color565(255, 90, 70),
                gptSessPct, gptWeekPct, gptWeekReset,
                cv.color565(255, 90, 70), cv.color565(255, 165, 40));

    // Nav dots
    int nsp = 24, ndy = H - 28;
    for (int i = 0; i < 3; i++) {
        int nx = cx + (i-1) * nsp;
        if (i == 1) cv.fillCircle(nx, ndy, 6, TFT_WHITE);
        else        cv.drawCircle(nx, ndy, 6, cv.color565(80, 80, 80));
    }
    cv.setTextDatum(TL_DATUM);
}
