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

// ── Usage screen — mirrors the claude.ai "Your usage limits" dialog ────────────
// Layout is anchored to the screen centre so it stays centred on any panel size
// and clear of the round bezel. Each limit row has its own colour.
// sessPct < 0 means no usage data has been received yet.
inline void renderUsageScreen(M5Canvas& cv,
                               int sessPct, int sessResetMin,
                               int weekAllPct, int weekSonPct,
                               const String& weekReset,
                               const String& model,
                               uint32_t sessionSec) {
    int W = cv.width(), H = cv.height();
    int cx = W / 2, cy = H / 2;
    cv.fillScreen(TFT_BLACK);

    // Per-row identity colours
    uint16_t accent = cv.color565(0, 150, 255);   // brand blue (header + session)
    uint16_t cSess  = cv.color565(0, 150, 255);   // session  — blue
    uint16_t cAll   = cv.color565(175, 120, 255); // weekly all — violet
    uint16_t cSon   = cv.color565(0, 205, 150);   // weekly sonnet — teal

    int halfDiv = (int)(W * 0.39f);
    int barL = cx - (int)(W * 0.37f);
    int barR = cx + (int)(W * 0.37f);
    int barH = 18;

    // ── Header: star + "Usage" centred, model name centred below ────────────────
    int iconX = cx - 48;
    _tkDrawStar(cv, iconX, cy - 128, 18, accent);
    cv.setFont(&fonts::Font4);
    cv.setTextDatum(ML_DATUM);
    cv.setTextColor(TFT_WHITE);
    cv.drawString("Usage", iconX + 26, cy - 128);

    if (model.length()) {
        cv.setFont(&fonts::Font2);
        cv.setTextDatum(MC_DATUM);
        cv.setTextColor(accent);
        cv.drawString(model.c_str(), cx, cy - 100);
    }

    cv.drawFastHLine(cx - halfDiv, cy - 82, 2 * halfDiv, cv.color565(36, 36, 56));

    // ── No data yet ─────────────────────────────────────────────────────────────
    if (sessPct < 0) {
        cv.setFont(&fonts::Font2);
        cv.setTextDatum(MC_DATUM);
        cv.setTextColor(cv.color565(120, 120, 150));
        cv.drawString("Waiting for usage data...", cx, cy);
        cv.setTextColor(cv.color565(90, 90, 115));
        cv.drawString("(needs claude.ai login)", cx, cy + 24);
        cv.setTextDatum(TL_DATUM);
        return;
    }

    char buf[16];

    // ── Current session ─────────────────────────────────────────────────────────
    snprintf(buf, sizeof(buf), "%d%%", sessPct);
    _tkBar(cv, barL, barR, cy - 64, barH, "Session", buf, sessPct / 100.0f, cSess);
    if (sessResetMin > 0) {
        char rbuf[24];
        if (sessResetMin >= 60)
            snprintf(rbuf, sizeof(rbuf), "resets in %dh %dm", sessResetMin / 60, sessResetMin % 60);
        else
            snprintf(rbuf, sizeof(rbuf), "resets in %dm", sessResetMin);
        _tkCaption(cv, cx, cy - 33, rbuf);
    }

    // ── Weekly limits ─────────────────────────────────────────────────────────────
    cv.setFont(&fonts::Font2);
    cv.setTextDatum(ML_DATUM);
    cv.setTextColor(cv.color565(130, 130, 160));
    cv.drawString("Weekly limits", barL, cy - 10);

    snprintf(buf, sizeof(buf), "%d%%", weekAllPct);
    _tkBar(cv, barL, barR, cy + 8, barH, "All", buf, weekAllPct / 100.0f, cAll);

    snprintf(buf, sizeof(buf), "%d%%", weekSonPct);
    _tkBar(cv, barL, barR, cy + 38, barH, "Sonnet", buf, weekSonPct / 100.0f, cSon);

    if (weekReset.length() > 0) {
        char rbuf[40];
        snprintf(rbuf, sizeof(rbuf), "resets %s", weekReset.c_str());
        _tkCaption(cv, cx, cy + 69, rbuf);
    }

    // ── Session duration footer ─────────────────────────────────────────────────
    cv.setFont(&fonts::Font2);
    cv.setTextDatum(MC_DATUM);
    cv.setTextColor(cv.color565(80, 80, 105));
    cv.drawString("session", cx, cy + 100);
    uint32_t sm = sessionSec / 60, ss = sessionSec % 60;
    char tbuf[12];
    snprintf(tbuf, sizeof(tbuf), "%dm%02ds", (int)sm, (int)ss);
    cv.setTextColor(cv.color565(120, 120, 150));
    cv.drawString(tbuf, cx, cy + 122);

    // ── Nav dots ────────────────────────────────────────────────────────────────
    int nsp = 24, ndy = H - 28;
    for (int i = 0; i < 3; i++) {
        int nx = cx + (i-1) * nsp;
        if (i == 1) cv.fillCircle(nx, ndy, 6, TFT_WHITE);
        else        cv.drawCircle(nx, ndy, 6, cv.color565(80, 80, 80));
    }
    cv.setTextDatum(TL_DATUM);
}
