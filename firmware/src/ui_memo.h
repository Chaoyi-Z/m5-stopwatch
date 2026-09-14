#pragma once
#include <M5Unified.h>
#include <math.h>

static inline void _memoDrawMic(M5Canvas& cv, int cx, int cy, uint16_t col) {
    cv.fillRoundRect(cx - 15, cy - 46, 30, 46, 15, col);
    cv.drawArc(cx, cy, 30, 22, 200, 340, col);
    cv.drawArc(cx, cy, 29, 23, 200, 340, col);
    cv.fillRect(cx - 2, cy + 30, 4, 14, col);
    cv.fillRect(cx - 16, cy + 44, 32, 4, col);
}

inline void renderMemoScreen(M5Canvas& cv,
                              bool memoRecording, bool memoPlaying,
                              size_t memoSamples, uint32_t animFrame,
                              float micLevel, float memoPlayFrac,
                              int volIdx = 2, int volCount = 4) {
    int W = cv.width(), H = cv.height();
    int cx = W/2, cy = H/2;
    cv.fillScreen(TFT_BLACK);

    if (memoRecording) {
        // ── Recording state ──────────────────────────────────────────────────
        bool blink = (animFrame / 6) % 2;
        uint16_t rc = cv.color565(255, 50, 50);
        // Pulsing outer ring
        cv.drawArc(cx, cy - 36, blink ? 46 : 38, blink ? 37 : 30, 0, 360, rc);
        // REC label
        cv.setFont(&fonts::Font4);
        cv.setTextDatum(MC_DATUM);
        cv.setTextColor(rc);
        cv.drawString("REC", cx, cy - 36);
        // Waveform bars
        for (int i = 0; i < 9; i++) {
            float w = (0.3f + 0.7f * micLevel)
                    * (0.4f + 0.6f * sinf(animFrame * 0.28f + i * 0.95f));
            int bh = 5 + (int)(30.0f * w);
            cv.fillRect(cx - 48 + i * 12, cy + 22 - bh/2, 9, bh,
                        cv.color565(220, 60 + (int)(80*w), 60));
        }
        // Duration counter
        float recSec = memoSamples / 16000.0f;
        char dbuf[10];
        snprintf(dbuf, sizeof(dbuf), "%.1fs", recSec);
        cv.setFont(&fonts::Font4);
        cv.setTextColor(cv.color565(200, 80, 80));
        cv.drawString(dbuf, cx, cy + 70);
        cv.setFont(&fonts::Font2);
        cv.setTextColor(cv.color565(140, 50, 50));
        cv.drawString("Release Yellow to stop", cx, cy + 98);

    } else if (memoPlaying) {
        // ── Playback state ───────────────────────────────────────────────────
        uint16_t pc = cv.color565(60, 210, 130);
        // Radiating arcs (animated)
        for (int r = 1; r <= 4; r++) {
            if (((animFrame / 3 + r) % 5) < 3)
                cv.drawArc(cx, cy - 28, r * 22, r * 22 - 6, 310, 410, pc);
        }
        cv.setFont(&fonts::Font4);
        cv.setTextDatum(MC_DATUM);
        cv.setTextColor(pc);
        cv.drawString("Playing", cx, cy - 28);
        // Progress bar
        int bY = cy + 28, bH = 12;
        cv.fillRoundRect(cx - 120, bY, 240, bH, 5, cv.color565(20, 48, 28));
        cv.fillRoundRect(cx - 120, bY, max(bH, (int)(240 * memoPlayFrac)), bH, 5, pc);
        // Time
        float total = memoSamples / 16000.0f;
        float elapsed = total * memoPlayFrac;
        char tbuf[20];
        snprintf(tbuf, sizeof(tbuf), "%.1f / %.1fs", elapsed, total);
        cv.setFont(&fonts::Font2);
        cv.setTextColor(cv.color565(80, 180, 110));
        cv.drawString(tbuf, cx, cy + 56);
        // Volume bars centred below time, with tap hints on each side
        int vbY = cy + 80;
        int totalW = volCount * 14 - 4;       // 4 bars × 14px gap, minus trailing gap
        int vbX = cx - totalW / 2;
        for (int i = 0; i < volCount; i++) {
            int bh = 6 + i * 5;
            uint16_t c = (i <= volIdx)
                       ? cv.color565(80, 220, 130)
                       : cv.color565(30, 60, 40);
            cv.fillRect(vbX + i * 14, vbY + (20 - bh), 10, bh, c);
        }
        cv.setFont(&fonts::Font2);
        cv.setTextColor(cv.color565(55, 110, 65));
        cv.setTextDatum(MR_DATUM);
        cv.drawString("< vol", vbX - 6, vbY + 10);
        cv.setTextDatum(ML_DATUM);
        cv.drawString("vol >", vbX + totalW + 6, vbY + 10);
        cv.setTextColor(cv.color565(70, 100, 70));
        cv.setTextDatum(MC_DATUM);
        cv.drawString("Blue: stop", cx, cy + 110);

    } else if (memoSamples > 0) {
        // ── Idle — has a recording ───────────────────────────────────────────
        _memoDrawMic(cv, cx, cy - 52, cv.color565(50, 180, 90));
        float totalSec = memoSamples / 16000.0f;
        char dbuf[16];
        snprintf(dbuf, sizeof(dbuf), "%.1fs", totalSec);
        cv.setFont(&fonts::Font6);
        cv.setTextDatum(MC_DATUM);
        cv.setTextColor(cv.color565(60, 210, 100));
        cv.drawString(dbuf, cx, cy + 22);
        cv.setFont(&fonts::Font2);
        cv.setTextColor(cv.color565(80, 80, 110));
        cv.drawString("Blue: replay", cx, cy + 66);
        cv.drawString("Yellow: re-record", cx, cy + 86);

    } else {
        // ── Idle — nothing recorded yet ──────────────────────────────────────
        _memoDrawMic(cv, cx, cy - 52, cv.color565(70, 70, 110));
        cv.setFont(&fonts::Font4);
        cv.setTextDatum(MC_DATUM);
        cv.setTextColor(cv.color565(100, 100, 145));
        cv.drawString("Hold Yellow", cx, cy + 22);
        cv.setFont(&fonts::Font2);
        cv.setTextColor(cv.color565(65, 65, 90));
        cv.drawString("to record", cx, cy + 50);
    }

    // Nav dots — 3 screens, right = active (index 2)
    int nsp = 24, ndy = H - 18;
    for (int i = 0; i < 3; i++) {
        int nx = cx + (i-1) * nsp;
        if (i == 2) cv.fillCircle(nx, ndy, 6, TFT_WHITE);
        else        cv.drawCircle(nx, ndy, 6, cv.color565(70, 70, 70));
    }
    cv.setTextDatum(TL_DATUM);
}
