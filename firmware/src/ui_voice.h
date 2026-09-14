#pragma once
#include <M5Unified.h>
#include <math.h>

inline void renderVoiceScreen(M5Canvas& cv, bool recording, bool transcribing,
                               const String& transcript, int batt,
                               uint32_t animFrame, float micLevel,
                               bool injected) {
    int W = cv.width(), H = cv.height();
    int cx = W/2, cy = H/2;

    cv.fillScreen(TFT_BLACK);

    // Battery
    uint16_t bc = (batt>20) ? cv.color565(80,220,80) : cv.color565(220,60,60);
    cv.drawRect(W-58, 14, 34, 16, TFT_WHITE);
    cv.fillRect(W-58+34, 18, 5, 8, TFT_WHITE);
    cv.fillRect(W-58+1, 15, max(1,(batt*32)/100), 14, bc);
    cv.setFont(&fonts::Font2); cv.setTextSize(1);
    cv.setTextColor(cv.color565(200,200,200));
    cv.setTextDatum(MR_DATUM);
    cv.drawString(String(batt)+"%", W-66, 22);
    cv.setTextDatum(TL_DATUM);

    if (recording) {
        // ── Recording: large pulsing red rings + waveform bars ────────────────
        uint16_t recColor = cv.color565(220, 40, 40);

        // Outer rings pulse large immediately (not dependent on micLevel)
        int phase = (animFrame / 3) % 16;   // 0-15, completes every ~2.4s
        int r1 = 70 + phase + (int)(20.0f * micLevel);
        int r2 = 95 + phase + (int)(20.0f * micLevel);
        cv.drawCircle(cx, cy-20, r2, cv.color565(100, 15, 15));
        cv.drawCircle(cx, cy-20, r2-2, cv.color565(80, 12, 12));
        cv.drawCircle(cx, cy-20, r1, cv.color565(180, 30, 30));
        cv.drawCircle(cx, cy-20, r1-2, cv.color565(160, 25, 25));
        // Solid mic circle (always big and red)
        cv.fillCircle(cx, cy-20, 50, recColor);
        cv.fillCircle(cx, cy-20, 17, TFT_WHITE);

        // Waveform bars (always animated, micLevel just scales height)
        int barY = cy + 64, bCount = 11, bSpacing = 14;
        int startX = cx - (bCount / 2) * bSpacing;
        float base = 0.35f + 0.65f * micLevel;   // minimum 0.35 so bars are always visible
        for (int i = 0; i < bCount; i++) {
            float wave = base * (0.4f + 0.6f * sinf(animFrame * 0.22f + i * 0.9f));
            int h = 10 + (int)(40.0f * wave);
            cv.fillRoundRect(startX + i * bSpacing - 3, barY - h / 2, 7, h, 3, recColor);
        }

        // Blinking "REC" badge — highly visible from frame 0
        bool blink = (animFrame / 6) % 2;
        cv.setFont(&fonts::Font4);
        cv.setTextDatum(MC_DATUM);
        cv.setTextColor(blink ? cv.color565(255, 80, 80) : cv.color565(160, 40, 40));
        cv.drawString("REC", cx, H - 60);
        cv.setFont(&fonts::Font2);
        cv.setTextColor(cv.color565(140, 60, 60));
        cv.drawString("Release Yellow to stop", cx, H - 28);
        cv.setTextDatum(TL_DATUM);

    } else if (transcribing) {
        // ── Waiting for STT result from bridge ────────────────────────────────
        int startA = (int)(animFrame * 4) % 360;
        cv.drawArc(cx, cy-20, 52, 40, startA, (startA+100)%360, cv.color565(60,160,255));
        cv.drawArc(cx, cy-20, 52, 40, (startA+180)%360, (startA+280)%360, cv.color565(20,60,140));

        cv.setFont(&fonts::Font4);
        cv.setTextDatum(MC_DATUM);
        cv.setTextColor(cv.color565(60,160,255));
        cv.drawString("Transcribing...", cx, cy+28);
        cv.setFont(&fonts::Font2);
        cv.setTextColor(cv.color565(60,80,120));
        cv.drawString("Sending to Google STT", cx, cy+58);
        cv.setTextDatum(TL_DATUM);

    } else if (transcript.length() > 0) {
        // ── Transcript received — waiting for Blue button ─────────────────────
        cv.setFont(&fonts::Font4);
        cv.setTextDatum(MC_DATUM);
        cv.setTextColor(cv.color565(60,210,60));
        cv.drawString(injected ? "Sent - ready to Enter" : "Transcribed:", cx, 72);

        // Word-wrap at ~20 chars per line
        cv.setFont(&fonts::Font4);
        cv.setTextColor(TFT_WHITE);
        String txt = transcript;
        int lineY = 124, lineH = 36;
        while (txt.length()>0 && lineY < H-90) {
            int cut = min(20,(int)txt.length());
            if (cut < (int)txt.length()) {
                int sp = txt.lastIndexOf(' ', cut);
                if (sp>0) cut=sp+1;
            }
            cv.drawString(txt.substring(0,cut), cx, lineY);
            txt = txt.substring(cut); txt.trim();
            lineY += lineH;
        }

        cv.setFont(&fonts::Font4);
        cv.setTextColor(cv.color565(60,140,255));
        cv.drawString(injected ? "Blue = Enter" : "Blue = Send to Claude", cx, H-54);
        cv.setFont(&fonts::Font2);
        cv.setTextColor(cv.color565(80,80,80));
        cv.drawString("Yellow = Record again", cx, H-28);
        cv.setTextDatum(TL_DATUM);

    } else {
        // ── Idle: prompt to start ─────────────────────────────────────────────
        cv.setFont(&fonts::Font6);
        cv.setTextDatum(MC_DATUM);
        cv.setTextColor(cv.color565(180,180,180));
        cv.drawString("Voice", cx, cy-40);
        cv.setFont(&fonts::Font4);
        cv.setTextColor(cv.color565(120,120,120));
        cv.drawString("Hold Yellow to record", cx, cy+20);
        cv.setTextDatum(TL_DATUM);
    }

    // Nav dots (right = active)
    int sp=24, dy=H-18;
    cv.drawCircle(cx-sp, dy, 6, cv.color565(70,70,70));
    cv.drawCircle(cx,    dy, 6, cv.color565(70,70,70));
    cv.fillCircle(cx+sp, dy, 6, TFT_WHITE);
}
