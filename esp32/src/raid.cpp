#include "raid.h"
#include "renderer.h"
#include <Arduino.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Raid 16 — boss showcase (M0.5). Five parametric face anatomies, each with
// its signature animations from the v0.2 spec. No sprite sheets: faces are
// drawn from state each tick, which is exactly how the M1 fight engine will
// drive them. Brightness discipline: dim 30–70 / body 200 / highlight 255.
// ---------------------------------------------------------------------------

#define RD_TICKMS 70

enum Boss : uint8_t { VANTA, MOTH, CHORUS, BULWARK, NULLK, NBOSS };

// Per-boss animation sets ('0'-'9' digits; 9 is always the death).
static const uint8_t ANIMS_VANTA[]   = {0,1,2,3,4,5,6,7,8,9};
static const uint8_t ANIMS_MOTH[]    = {0,1,2,3,4,5,9};
static const uint8_t ANIMS_CHORUS[]  = {0,1,2,3,4,9};
static const uint8_t ANIMS_BULWARK[] = {0,1,2,3,4,9};
static const uint8_t ANIMS_NULL[]    = {0,1,2,3,4,9};
static const uint8_t* ANIM_SET[NBOSS] = {ANIMS_VANTA, ANIMS_MOTH, ANIMS_CHORUS, ANIMS_BULWARK, ANIMS_NULL};
static const uint8_t  ANIM_N[NBOSS]   = {10, 7, 6, 6, 6};

static int      rdW, rdH, rdFaceH, rdStageY;   // stageY = -1 when no stage panel
static uint8_t  rdBoss, rdAnim, rdAnimIdx;
static bool     rdAuto;
static uint16_t rdT;
static uint32_t rdLast;
static float    rdPY[6]; static int8_t rdPX[6];   // particle pool
static uint8_t  rdMelt[16];
static bool     fOn = false;                       // fight mode active (engine below)

static void rdReset(uint8_t idx) {
    rdAnimIdx = idx % ANIM_N[rdBoss];
    rdAnim = ANIM_SET[rdBoss][rdAnimIdx];
    rdT = 0;
    for (int i = 0; i < 6; i++) { rdPY[i] = -(float)(i * 5); rdPX[i] = 0; }
    memset(rdMelt, 0, sizeof(rdMelt));
}

static void rdSetBoss(uint8_t b) { rdBoss = b % NBOSS; rdReset(0); }

void raidInit(Renderer& r) {
    rdW = r.width(); rdH = r.height();
    rdFaceH = rdH < 16 ? rdH : 16;
    rdStageY = rdH >= 24 ? 16 : -1;
    rdAuto = true; rdLast = 0;
    rdSetBoss(VANTA);
}

static bool fightInput(char k);   // fwd (fight code sits below the showcase)

void raidInput(uint8_t, char k, bool pressed) {
    if (!pressed) return;
    if (k == 'G' || fOn) { fightInput(k); return; }
    switch (k) {
        case 'V': rdSetBoss(VANTA);   return;
        case 'M': rdSetBoss(MOTH);    return;
        case 'C': rdSetBoss(CHORUS);  return;
        case 'B': rdSetBoss(BULWARK); return;
        case 'N': rdSetBoss(NULLK);   return;
    }
    if (k >= '0' && k <= '9') {
        for (uint8_t i = 0; i < ANIM_N[rdBoss]; i++)
            if (ANIM_SET[rdBoss][i] == k - '0') { rdAuto = false; rdReset(i); return; }
        return;                                   // digit not in this boss's set
    }
    if (k == 'R' || k == 'A' || k == 'U') { rdAuto = false; rdReset(rdAnimIdx + 1); }
    if (k == 'L' || k == 'D')             { rdAuto = false; rdReset(rdAnimIdx + ANIM_N[rdBoss] - 1); }
}

// --- shared face parts (standard anatomy: Vanta, and Bulwark underneath) ---
static void rdEyes(Renderer& r, int ex, bool closed, int pupilDx) {
    for (int e = 0; e < 2; e++) {
        int bx = (e ? 10 : 3) + ex;
        if (closed) { for (int x = 0; x < 3; x++) r.setPixel(bx + x, 4, 200); continue; }
        for (int y = 3; y <= 5; y++)
            for (int x = 0; x < 3; x++) r.setPixel(bx + x, y, 200);
        int px = bx + 1 + pupilDx; if (px < bx) px = bx; if (px > bx + 2) px = bx + 2;
        r.setPixel(px, 4, 0);
    }
}
static void rdBrow(Renderer& r, int ex, bool angry) {
    for (int e = 0; e < 2; e++) {
        int bx = (e ? 10 : 3) + ex;
        for (int x = 0; x < 3; x++) {
            int y = angry ? (e ? 1 + (2 - x) / 2 : 1 + x / 2) : 1;
            r.setPixel(bx + x, y, 255);
        }
    }
}
static void rdMouth(Renderer& r, int stage, bool grin) {
    if (grin) {
        for (int x = 3; x <= 12; x++) r.setPixel(x, 11 + ((x & 1) ? 1 : 0), 255);
        return;
    }
    if (stage <= 0) { for (int x = 5; x <= 10; x++) r.setPixel(x, 12, 200); return; }
    int hw = 2 + stage;
    int top = 12 - stage, bot = 12 + (stage > 1 ? 1 : 0);
    for (int y = top; y <= bot; y++)
        for (int x = 8 - hw; x <= 7 + hw; x++)
            r.setPixel(x, y, y == top || y == bot ? 200 : 90);
}
static void rdNoise(Renderer& r, int pct, int hLimit) {
    int n = rdW * hLimit * pct / 100;
    for (int i = 0; i < n; i++) {
        int x = esp_random() % rdW, y = esp_random() % hLimit;
        r.setPixel(x, y, r.getPixel(x, y) > 100 ? 0 : 200);
    }
}
static void rdStage(Renderer& r, int hullSeg, bool burning) {
    if (rdStageY < 0) return;
    int by = rdStageY + 1;
    for (int s = 0; s < 8; s++) {
        if (burning && s == hullSeg && (rdT & 2)) { r.setPixel(1 + s * 2, by, 255); continue; }
        if (s < hullSeg) r.setPixel(1 + s * 2, by, 200);
    }
    for (int l = 0; l < 4; l++) r.setPixel(2 + l * 4, rdH - 1, 45);
}

// ============================ VANTA (standard) =============================
static void tickVanta(Renderer& r) {
    switch (rdAnim) {
    case 0: {                                    // idle — cold, watching
        int ex = (rdT / 25) % 3 - 1;
        bool blink = (rdT % 43) < 2 || (rdT % 43) == 4;
        rdBrow(r, ex, false); rdEyes(r, ex, blink, ex);
        rdMouth(r, 0, false); rdStage(r, 8, false);
        break;
    }
    case 1: case 2: {                            // sweep telegraph L / R
        int dir = rdAnim == 1 ? -1 : 1;
        int edge = dir < 0 ? 0 : rdW - 1;
        rdBrow(r, dir * 2, true); rdEyes(r, dir * 2, false, dir);
        rdMouth(r, 0, false);
        if (rdT < 30) {
            if (rdT & 1) for (int y = 0; y < rdFaceH; y++) r.setPixel(edge, y, 255);
        } else {
            int p = (rdT - 30) * 2;
            int x = dir < 0 ? rdW - 1 - p : p;
            for (int y = 0; y < rdFaceH; y++) { r.setPixel(x, y, 255); r.setPixel(x - dir, y, 90); }
            if (p > rdW + 2) rdReset(rdAnimIdx);
        }
        rdStage(r, 8, false);
        break;
    }
    case 3: {                                    // beam — blink code (freq 3)
        int ph = rdT % 34;
        bool closed = (ph < 12) && ((ph % 4) < 2);
        rdBrow(r, 0, true); rdEyes(r, 0, closed, 0);
        rdMouth(r, 0, false);
        if (ph >= 26) for (int y = 0; y < rdH; y++) { r.setPixel(7, y, 255); r.setPixel(8, y, 255); }
        rdStage(r, 7, false);
        break;
    }
    case 4: {                                    // charge blast
        int stage = 1 + (rdT / 18); if (stage > 3) stage = 3;
        rdBrow(r, 0, true); rdEyes(r, 0, false, 0);
        rdMouth(r, stage, false);
        if (rdT > 58 && rdT < 63)
            for (int y = 0; y < rdFaceH; y++) for (int x = 0; x < rdW; x++) r.setPixel(x, y, 255);
        if (rdT >= 68) rdReset(rdAnimIdx);
        rdStage(r, 6, false);
        break;
    }
    case 5: {                                    // acid spew
        rdBrow(r, 0, true); rdEyes(r, 0, false, 0);
        rdMouth(r, 2, false);
        for (int i = 0; i < 3; i++) {
            rdPY[i] += 0.8f;
            if (rdPY[i] > rdH - 1) { rdPY[i] = 13; rdPX[i] = 6 + esp_random() % 4; }
            if (rdPY[i] >= 13 && rdPY[i] <= rdH - 2)
                r.setPixel(rdPX[i] ? rdPX[i] : 7, (int)rdPY[i], 255);
        }
        if (rdStageY >= 0 && (rdT & 2)) r.setPixel(2 + 1 * 4, rdH - 1, 255);
        rdStage(r, 6, false);
        break;
    }
    case 6: {                                    // jam — static ramps
        int jx = (esp_random() % 3) - 1;
        rdBrow(r, jx, false); rdEyes(r, jx, false, jx);
        rdMouth(r, 0, false);
        rdNoise(r, rdT < 40 ? rdT : 40, rdFaceH);
        rdStage(r, 6, false);
        break;
    }
    case 7: {                                    // enrage
        rdBrow(r, 0, true); rdEyes(r, 0, false, (rdT & 4) ? 1 : -1);
        rdMouth(r, 0, true);
        rdNoise(r, 6, rdFaceH);
        rdStage(r, 5, true);
        break;
    }
    case 8: {                                    // bullet hell (stage)
        int ex = ((rdT / 6) % 3) - 1;
        rdBrow(r, ex, true); rdEyes(r, ex, false, ex);
        rdMouth(r, 1, false);
        if (rdStageY >= 0)
            for (int i = 0; i < 4; i++) {
                rdPY[i] += 0.9f + i * 0.12f;
                if (rdPY[i] > rdH - 1 || rdPY[i] < 0) rdPY[i] = rdStageY;
                int x = 2 + i * 4, y = (int)rdPY[i];
                if (y >= rdStageY) { r.setPixel(x, y, 255); if (y - 1 >= rdStageY) r.setPixel(x, y - 1, 70); }
            }
        rdStage(r, 4, true);
        break;
    }
    case 9: {                                    // death melt
        for (int x = 0; x < rdW && x < 16; x++)
            if ((esp_random() % 5) == 0 && rdMelt[x] < rdH) rdMelt[x]++;
        for (int e = 0; e < 2; e++) {
            int bx = e ? 10 : 3;
            for (int y = 3; y <= 5; y++)
                for (int x = 0; x < 3; x++) {
                    int cy = y + rdMelt[bx + x];
                    if (cy < rdH && !(y == 4 && x == 1)) r.setPixel(bx + x, cy, 200);
                }
        }
        for (int x = 5; x <= 10; x++) {
            int cy = 12 + rdMelt[x];
            if (cy < rdH) r.setPixel(x, cy, 200);
        }
        for (int x = 0; x < rdW && x < 16; x++)
            if (rdMelt[x] > 4) r.setPixel(x, rdH - 1, 70);
        if (rdT > 80) rdReset(rdAuto ? 0 : rdAnimIdx);
        break;
    }
    }
}

// ============================ MOTH (compound) ==============================
// Two 4x4 compound-eye grids of independently blinking cells, antennae,
// wing columns at the panel edges, scissoring mandibles.
static void mothFace(Renderer& r, int flutterAmp, bool angry) {
    for (int e = 0; e < 2; e++) {                // compound eyes
        int bx = e ? 10 : 2;
        for (int cy = 0; cy < 4; cy++)
            for (int cx = 0; cx < 4; cx++) {
                uint16_t h = (uint16_t)((cx * 7 + cy * 13 + e * 5 + rdT / 3) * 2654435u);
                r.setPixel(bx + cx, 2 + cy, (h % 5) ? 200 : 0);
            }
    }
    // antennae: two arcs above, tips alternate full-bright
    r.setPixel(4, 1, 200); r.setPixel(3, 0, (rdT & 4) ? 255 : 90);
    r.setPixel(11, 1, 200); r.setPixel(12, 0, (rdT & 4) ? 90 : 255);
    // wings: edge columns flutter
    int amp = flutterAmp;
    for (int y = 3; y <= 12; y++) {
        bool beat = ((rdT + y) / 2) & 1;
        if (amp > 0 && beat) { r.setPixel(0, y, 70); r.setPixel(15, y, 70); }
        if (amp > 1)         { r.setPixel(1, y, beat ? 45 : 70); r.setPixel(14, y, beat ? 70 : 45); }
    }
    // mandibles scissor
    int mo = (rdT / 4) & 1;
    r.setPixel(7 - mo, 13, angry ? 255 : 200); r.setPixel(8 + mo, 13, angry ? 255 : 200);
}
static void tickMoth(Renderer& r) {
    switch (rdAnim) {
    case 0:                                      // idle — never still
        mothFace(r, 2, false); rdStage(r, 8, false); break;
    case 1: {                                    // FEINT — dissolves at 60%
        mothFace(r, 1, true);
        int edge = rdW - 1;
        if (rdT < 22) {                          // looks exactly like a real tell…
            if (rdT & 1) for (int y = 0; y < rdFaceH; y++) r.setPixel(edge, y, 255);
        } else if (rdT < 30) {                   // …then dissolves into motes
            for (int y = 0; y < rdFaceH; y += 2)
                if ((y + rdT) & 3) r.setPixel(edge - (rdT - 22) / 2, y, 45);
        } else if (rdT > 40) rdReset(rdAnimIdx);
        rdStage(r, 8, false);
        break;
    }
    case 2: {                                    // real sweep (compare with 1)
        mothFace(r, 1, true);
        int edge = rdW - 1;
        if (rdT < 22) {
            if (rdT & 1) for (int y = 0; y < rdFaceH; y++) r.setPixel(edge, y, 255);
        } else {
            int p = (rdT - 22) * 2;
            int x = rdW - 1 - p;
            for (int y = 0; y < rdFaceH; y++) { r.setPixel(x, y, 255); r.setPixel(x + 1, y, 90); }
            if (p > rdW + 2) rdReset(rdAnimIdx);
        }
        rdStage(r, 8, false);
        break;
    }
    case 3: {                                    // DUST — motes bury the stage
        mothFace(r, 3, false);
        for (int i = 0; i < 6; i++) {
            rdPY[i] += 0.25f + (i % 3) * 0.1f;
            rdPX[i] = (int8_t)((rdPX[i] + ((rdT + i) & 2 ? 1 : 0)) % rdW);
            if (rdPY[i] > rdH - 1) { rdPY[i] = rdStageY >= 0 ? rdStageY : 8; rdPX[i] = esp_random() % rdW; }
            if (rdPY[i] >= 0) r.setPixel(rdPX[i], (int)rdPY[i], 45);
        }
        rdStage(r, 7, false);                    // lane markers get lost in the motes
        break;
    }
    case 4: {                                    // FLURRY — rapid weak beams
        mothFace(r, 2, true);
        int ph = rdT % 12;
        if (ph >= 8) {
            int col = 2 + ((rdT / 12) * 5) % 12;
            for (int y = 6; y < rdH; y++) r.setPixel(col, y, ph == 8 ? 255 : 90);
        }
        rdStage(r, 7, false);
        break;
    }
    case 5:                                      // FRENZY (enrage)
        mothFace(r, 3, true);
        rdNoise(r, 5, rdFaceH);
        rdStage(r, 5, true);
        break;
    case 9: {                                    // death — disintegrates upward
        int gone = rdT;                          // cells release bottom-up
        for (int e = 0; e < 2; e++) {
            int bx = e ? 10 : 2;
            for (int cy = 0; cy < 4; cy++)
                for (int cx = 0; cx < 4; cx++) {
                    int release = (3 - cy) * 8 + ((cx * 5 + e * 3) % 8);
                    if (gone < release) r.setPixel(bx + cx, 2 + cy, 200);
                }
        }
        for (int i = 0; i < 6; i++) {            // freed motes flutter up and away
            rdPY[i] -= 0.5f + (i % 2) * 0.2f;
            if (rdPY[i] < -1) rdPY[i] = 6 + esp_random() % 6;
            int x = (2 + i * 2 + ((rdT + i) & 3)) % rdW;
            if (rdPY[i] >= 0) r.setPixel(x, (int)rdPY[i], 70);
        }
        if (rdT > 70) rdReset(rdAuto ? 0 : rdAnimIdx);
        break;
    }
    }
}

// =========================== CHORUS (triple-band) ==========================
// Three stacked 5-row mini-faces. Band y-origins: 0, 5, 10.
static void bandFace(Renderer& r, int by, int ex, bool grin, bool dead, bool lead) {
    if (dead) {                                  // hollow outline, silenced
        for (int x = 4; x <= 11; x += 2) r.setPixel(x, by + 2, 30);
        return;
    }
    for (int e = 0; e < 2; e++) {                // 3x2 eyes with pupil
        int bx = (e ? 10 : 3) + ex;
        for (int y = 1; y <= 2; y++)
            for (int x = 0; x < 3; x++) r.setPixel(bx + x, by + y, 200);
        r.setPixel(bx + 1 + (ex > 0 ? 1 : ex < 0 ? -1 : 0), by + 1, 0);
    }
    if (grin) { for (int x = 5; x <= 10; x++) r.setPixel(x, by + 4 - (x & 1), 255); }
    else      { for (int x = 6; x <= 9;  x++) r.setPixel(x, by + 4, 200); }
    if (lead) for (int y = 0; y < 5; y++) {      // leading head's band edges glow
        r.setPixel(0, by + y, (rdT & 2) ? 255 : 90);
        r.setPixel(15, by + y, (rdT & 2) ? 255 : 90);
    }
}
static void tickChorus(Renderer& r) {
    switch (rdAnim) {
    case 0: {                                    // idle — they bicker
        int g0 = ((rdT / 20) % 3) - 1, g1 = ((rdT / 26 + 1) % 3) - 1, g2 = ((rdT / 17 + 2) % 3) - 1;
        bandFace(r, 0, g0, false, false, false);
        bandFace(r, 5, g1, false, false, false);
        bandFace(r, 10, g2, false, false, false);
        rdStage(r, 8, false);
        break;
    }
    case 1: {                                    // ROUND — chain T->M->B
        int step = (rdT / 18) % 4;               // 0,1,2 = heads, 3 = rest
        for (int h = 0; h < 3; h++)
            bandFace(r, h * 5, step == h ? 2 : 0, step == h, false, step == h);
        if (step < 3) {                          // order arrows tick across the lead band
            int ax = (rdT % 18);
            if (ax < 16) r.setPixel(ax, step * 5 + 2, 255);
        }
        rdStage(r, 7, false);
        break;
    }
    case 2: {                                    // lead change — glow migrates
        int lead = (rdT / 30) % 3;
        for (int h = 0; h < 3; h++) bandFace(r, h * 5, 0, false, false, h == lead);
        rdStage(r, 8, false);
        break;
    }
    case 3: {                                    // middle head down — survivors harmonize
        bandFace(r, 0, (rdT / 8) % 3 - 1, false, false, (rdT & 8) != 0);
        bandFace(r, 5, 0, false, true, false);
        bandFace(r, 10, (rdT / 8 + 1) % 3 - 1, false, false, (rdT & 8) == 0);
        rdStage(r, 6, false);
        break;
    }
    case 4: {                                    // unison grin — the only agreement
        for (int h = 0; h < 3; h++) bandFace(r, h * 5, 0, true, false, false);
        rdStage(r, 6, false);
        break;
    }
    case 9: {                                    // death — pop, pop… the last looks around
        bool d0 = rdT > 15, d1 = rdT > 35, last = !d1;
        bandFace(r, 0, 0, false, d0, false);
        bandFace(r, 5, 0, false, d1, false);
        if (rdT <= 55) {
            int gaze = rdT > 35 ? (((rdT / 6) & 1) ? -1 : 1) : 0;   // searching for its siblings
            bandFace(r, 10, gaze, false, false, false);
        } else if (rdT <= 70) {                  // deflates
            int sink = (rdT - 55) / 5;
            for (int x = 6 - sink; x <= 9 + sink && x < 16 && x >= 0; x++)
                if (14 < rdFaceH) r.setPixel(x, 14, 90);
        }
        (void)last;
        if (rdT > 80) rdReset(rdAuto ? 0 : rdAnimIdx);
        break;
    }
    }
}

// =========================== BULWARK (visor) ===============================
// Standard face behind horizontal armor slats; lift exposes it row by row.
static void visor(Renderer& r, int liftRows) {
    for (int y = rdFaceH - 1; y >= liftRows; y--) {
        int vy = y - liftRows;                   // slat pattern scrolls with the lift
        bool slat = (vy % 3) != 2;
        for (int x = 1; x <= 14; x++)
            if (slat) r.setPixel(x, y, 90);
    }
    if (liftRows < 5) {                          // eye slits shine through
        for (int x = 3; x <= 5; x++)  r.setPixel(x, 4, 255);
        for (int x = 10; x <= 12; x++) r.setPixel(x, 4, 255);
    }
}
static void tickBulwark(Renderer& r) {
    switch (rdAnim) {
    case 0: {                                    // idle — slits track; it knocks
        visor(r, 0);
        int kn = rdT % 60;
        if (kn == 40 || kn == 44) { r.setPixel(7, 8, 255); r.setPixel(8, 8, 255); }
        rdStage(r, 8, false);
        break;
    }
    case 1: {                                    // RIPOSTE — fast counter tell
        visor(r, 0);
        if (rdT < 8) {                           // slits flare
            for (int x = 2; x <= 13; x++) r.setPixel(x, 4, 255);
        } else if (rdT < 16) {                   // counter sweep, twice the speed
            int p = (rdT - 8) * 3;
            for (int y = 0; y < rdFaceH; y++) r.setPixel(p < rdW ? p : rdW - 1, y, 255);
        } else if (rdT > 26) rdReset(rdAnimIdx);
        rdStage(r, 7, false);
        break;
    }
    case 2: {                                    // VISOR LIFT — the window
        int lift = rdT < 24 ? rdT / 3 : (rdT > 60 ? (72 - rdT) / 2 : 8);
        if (lift < 0) lift = 0; if (lift > 8) lift = 8;
        rdBrow(r, 0, false); rdEyes(r, 0, false, 0); rdMouth(r, 1, false);
        visor(r, lift * 2);
        if (lift >= 8 && (rdT & 2)) {            // vulnerable pulse
            r.setPixel(0, 12, 255); r.setPixel(15, 12, 255);
        }
        if (rdT > 74) rdReset(rdAnimIdx);
        rdStage(r, 7, false);
        break;
    }
    case 3: {                                    // BASH — the screen shudders
        int shove = (rdT % 30) < 4 ? ((rdT % 30) < 2 ? 1 : -1) : 0;
        for (int y = rdFaceH - 1; y >= 0; y--) {
            int vy = y; bool slat = (vy % 3) != 2;
            for (int x = 1; x <= 14; x++)
                if (slat) r.setPixel(x + shove, y, 90);
        }
        for (int x = 3; x <= 5; x++)  r.setPixel(x + shove, 4, 255);
        for (int x = 10; x <= 12; x++) r.setPixel(x + shove, 4, 255);
        if ((rdT % 30) < 4 && rdStageY >= 0)     // impact ring on the stage
            for (int x = 0; x < rdW; x += 3) r.setPixel(x, rdStageY, 255);
        rdStage(r, 6, false);
        break;
    }
    case 4: {                                    // MOCKING fake lift
        int ph = rdT % 44;
        int lift = ph < 12 ? ph / 3 : (ph < 16 ? 4 - (ph - 12) : 0);   // peeks… slams
        rdEyes(r, 0, false, 0);
        visor(r, lift * 2);
        if (ph == 16 && rdStageY >= 0)           // slam clang
            for (int x = 0; x < rdW; x += 2) r.setPixel(x, rdStageY, 255);
        rdStage(r, 8, false);
        break;
    }
    case 9: {                                    // death — visor falls off, face crumbles
        if (rdT < 30) {                          // visor slides down and off
            int drop = rdT / 2;
            for (int y = 0; y < rdFaceH; y++) {
                int vy = y + drop;
                if (vy >= rdH) continue;
                if ((y % 3) != 2) for (int x = 1; x <= 14; x++) r.setPixel(x, vy, 90);
            }
            rdBrow(r, 0, false); rdEyes(r, 0, false, 0); rdMouth(r, 2, false);   // surprised
        } else {                                 // crumble from the edges inward
            int c = (rdT - 30) / 4;
            rdBrow(r, 0, false); rdEyes(r, 0, false, 0); rdMouth(r, 2, false);
            for (int y = 0; y < rdFaceH; y++)
                for (int x = 0; x < rdW; x++)
                    if (x < c || x >= rdW - c || (esp_random() % 10) < c)
                        r.setPixel(x, y, 0);
        }
        if (rdT > 75) rdReset(rdAuto ? 0 : rdAnimIdx);
        break;
    }
    }
}

// ============================ NULL (noise) =================================
// A face that only barely coheres out of static. cohere 0..100.
static void nullFace(Renderer& r, int cohere) {
    rdBrow(r, 0, true); rdEyes(r, 0, false, 0); rdMouth(r, 0, false);
    for (int y = 0; y < rdFaceH; y++)            // erode the face by (100-cohere)%
        for (int x = 0; x < rdW; x++)
            if (r.getPixel(x, y) > 0 && (int)(esp_random() % 100) > cohere)
                r.setPixel(x, y, 0);
    rdNoise(r, 25 - cohere / 5, rdFaceH);        // and a static floor underneath
}
static void tickNull(Renderer& r) {
    switch (rdAnim) {
    case 0: {                                    // idle — static breathes
        int breathe = 30 + (int)(30.0f * (0.5f + 0.5f * sinf(rdT * 0.08f)));
        // once in a while it wears another boss's face for a second
        if ((rdT % 90) > 80) { bandFace(r, 5, 0, false, false, false); rdNoise(r, 20, rdFaceH); }
        else nullFace(r, breathe);
        rdStage(r, 8, false);
        break;
    }
    case 1: {                                    // cohere + beam — it becomes real to hurt you
        int c = rdT < 25 ? rdT * 4 : 100;
        nullFace(r, c > 100 ? 100 : c);
        if (rdT >= 30 && rdT < 40)
            for (int y = 0; y < rdH; y++) { r.setPixel(7, y, 255); r.setPixel(8, y, 255); }
        if (rdT > 46) rdReset(rdAnimIdx);
        rdStage(r, 7, false);
        break;
    }
    case 2: {                                    // POSSESSION — the eye leaves the screen
        nullFace(r, 35);
        // one huge eye forms center-screen then "transmits" downward off the panel
        int ph = rdT % 60;
        if (ph < 30) {
            int rr = ph < 10 ? ph / 3 : 3;
            for (int dy = -rr; dy <= rr; dy++)
                for (int dx = -rr; dx <= rr; dx++)
                    if (dx * dx + dy * dy <= rr * rr) r.setPixel(8 + dx, 7 + dy, 200);
            if (rr >= 2) r.setPixel(8, 7, 0);    // its pupil
        } else {                                 // beams down toward a deck lane
            int y0 = 7 + (ph - 30);
            if (y0 < rdH) { r.setPixel(8, y0, 255); r.setPixel(8, y0 - 1, 90); }
            if (rdStageY >= 0 && (rdT & 2)) r.setPixel(2 + 2 * 4, rdH - 1, 255);
        }
        rdStage(r, 6, false);
        break;
    }
    case 3: {                                    // INVERSION — read it upside down
        // draw the idle face into the top, then flip the face region vertically
        nullFace(r, 70);
        for (int y = 0; y < rdFaceH / 2; y++)
            for (int x = 0; x < rdW; x++) {
                uint8_t a = r.getPixel(x, y), b = r.getPixel(x, rdFaceH - 1 - y);
                r.setPixel(x, y, b); r.setPixel(x, rdFaceH - 1 - y, a);
            }
        rdStage(r, 6, false);
        break;
    }
    case 4: {                                    // FALSE WIPE — never celebrate early
        if (rdT < 35) {                          // a perfect copy of the death melt…
            for (int x = 0; x < rdW && x < 16; x++)
                if ((esp_random() % 4) == 0 && rdMelt[x] < rdH) rdMelt[x]++;
            for (int e = 0; e < 2; e++) {
                int bx = e ? 10 : 3;
                for (int y = 3; y <= 5; y++)
                    for (int x = 0; x < 3; x++) {
                        int cy = y + rdMelt[bx + x];
                        if (cy < rdH && !(y == 4 && x == 1)) r.setPixel(bx + x, cy, 200);
                    }
            }
        } else if (rdT < 50) {                   // …black. you cheer. then—
            // (empty screen)
        } else if (rdT < 58) {                   // it snaps back all at once
            nullFace(r, (rdT - 50) * 12);
        } else {
            nullFace(r, 100);
            rdMouth(r, 3, false);                // instant charge, no telegraph
            if (rdT > 66) rdReset(rdAnimIdx);
        }
        rdStage(r, rdT < 35 ? 5 : 5, false);
        break;
    }
    case 9: {                                    // true death — CRT off
        if (rdT < 40) {                          // static collapses inward
            int rr = 10 - rdT / 4;
            for (int i = 0; i < 40; i++) {
                int dx = (int)(esp_random() % 21) - 10, dy = (int)(esp_random() % 21) - 10;
                if (dx * dx + dy * dy <= rr * rr) {
                    int x = 8 + dx, y = 8 + dy;
                    if (x >= 0 && x < rdW && y >= 0 && y < rdFaceH) r.setPixel(x, y, 200);
                }
            }
        } else if (rdT < 48) {
            if (rdT & 2) r.setPixel(8, 8, 255);  // the last pixel blinks
        } else if (rdT >= 90 && rdT < 92) {
            r.setPixel(8, 8, 90);                // …one final faint blink from the dark
        }
        if (rdT > 100) rdReset(rdAuto ? 0 : rdAnimIdx);
        break;
    }
    }
}

// ======================= M1 slice: the VANTA fight =========================
// Solo-sim rules (docs/raid16.md §4): no decode layer — the boss blinks its
// beam frequency openly; read the panel, work the deck. Keys ride the normal
// input path: G start/restart, Q quit, L/R shield side, 1-4 freq dial,
// O overcharge, K crank (+10%), F fire. Boss HP bar = top stage row; hull =
// the 8-segment bar below it. Beat the enrage timer or grin-wipe.
enum FPhase : uint8_t { F_GAP, F_TELE, F_RESOLVE, F_WIN, F_LOSE };
static uint8_t  fPhase, fType;          // 0 sweepL 1 sweepR 2 beam 3 charge
static uint8_t  fFreq;                  // beam frequency 1..4
static int16_t  fHP; static int8_t fHull;
static uint16_t fPT, fGapT, fWin;       // phase ticks, gap length, window length
static uint8_t  fSide, fDial;           // latched responses (side clears; dial persists)
static bool     fOver, fOk;
static uint8_t  fCrank;
static uint16_t fVuln, fEnrage;
static uint8_t  fFxShot;

static void fightStart() {
    fOn = true; fPhase = F_GAP; fPT = 0; fGapT = 30;
    fHP = 100; fHull = 8; fSide = 0; fDial = 0; fOver = false;
    fCrank = 0; fVuln = 0; fEnrage = 1700;      // ~2 min at 70 ms/tick
    fFxShot = 0;
    memset(rdMelt, 0, sizeof(rdMelt));
}
static void fireShot() {
    if (fCrank < 100) return;
    fCrank = 0; fFxShot = 4;
    if (fPhase == F_TELE && fType == 3) {        // charge interrupt!
        fHP -= 4; fOk = true; fPhase = F_RESOLVE; fPT = 0; return;
    }
    fHP -= (fVuln > 0) ? 9 : 3;
}
static bool fightInput(char k) {
    if (k == 'G') { fightStart(); return true; }
    if (!fOn) return false;
    if (k == 'Q') { fOn = false; rdReset(0); return true; }
    if (k == 'L') { fSide = 1; return true; }
    if (k == 'R') { fSide = 2; return true; }
    if (k >= '1' && k <= '4') { fDial = k - '0'; return true; }
    if (k == 'O') { fOver = true; return true; }
    if (k == 'K') { fCrank = fCrank > 90 ? 100 : fCrank + 10; return true; }
    if (k == 'F') { fireShot(); return true; }
    return true;                                 // swallow everything else mid-fight
}
static void fightDrawBars(Renderer& r) {
    if (rdStageY < 0) return;
    int lit = fHP > 0 ? (fHP * rdW + 99) / 100 : 0;   // boss HP across the top stage row
    for (int x = 0; x < lit && x < rdW; x++)
        r.setPixel(x, rdStageY, fVuln > 0 && (rdT & 2) ? 255 : 200);
    rdStage(r, fHull, fEnrage < 300);            // hull + burning frame near enrage
    if (fCrank >= 100 && (rdT & 2)) r.setPixel(rdW - 1, rdH - 1, 255);   // "gun hot" pip
}
static void fightTick(Renderer& r) {
    fPT++;
    if (fVuln) fVuln--;
    if (fFxShot) { fFxShot--;                    // muzzle flash column at the boss
        for (int y = 6; y < rdFaceH; y++) r.setPixel(12, y, 255); }
    if (fPhase != F_WIN && fPhase != F_LOSE) {
        if (fHP <= 0)  { fPhase = F_WIN;  fPT = 0; }
        if (fHull <= 0 || --fEnrage == 0) { fPhase = F_LOSE; fPT = 0; }
    }
    switch (fPhase) {
    case F_GAP:
        rdBrow(r, 0, false); rdEyes(r, 0, (fPT % 40) < 2, 0);
        rdMouth(r, fVuln > 0 ? 2 : 0, false);    // vuln: mouth hangs open — shoot it
        if (fPT >= fGapT) {
            fType = esp_random() % 4;
            fFreq = 1 + esp_random() % 4;
            fWin  = fType == 2 ? 57 : (fType == 3 ? 43 : 36);
            fSide = 0; fOver = false;
            fPhase = F_TELE; fPT = 0;
        }
        break;
    case F_TELE: {
        if (fType <= 1) {                        // sweep: eyes + edge ripple
            int dir = fType == 0 ? -1 : 1;
            rdBrow(r, dir * 2, true); rdEyes(r, dir * 2, false, dir);
            rdMouth(r, 0, false);
            if (fPT & 1) { int e = dir < 0 ? 0 : rdW - 1;
                for (int y = 0; y < rdFaceH; y++) r.setPixel(e, y, 255); }
        } else if (fType == 2) {                 // beam: blink code = the frequency
            int cyc = fFreq * 8 + 14, ph = fPT % cyc;
            bool closed = ph < fFreq * 8 && (ph % 8) < 4;
            rdBrow(r, 0, true); rdEyes(r, 0, closed, 0); rdMouth(r, 0, false);
        } else {                                 // charge: mouth opens on a timer
            rdBrow(r, 0, true); rdEyes(r, 0, false, 0);
            rdMouth(r, 1 + fPT * 3 / fWin, false);
        }
        if (fPT >= fWin) {
            if      (fType <= 1) fOk = fSide == (fType == 0 ? 1 : 2);
            else if (fType == 2) fOk = fDial == fFreq;
            else                 fOk = fOver;
            if (fOk && fType == 2) fVuln = 43;   // clean beam block opens the window
            if (!fOk) { fHull -= (fType == 2 ? 2 : fType == 3 ? 3 : 1); }
            fPhase = F_RESOLVE; fPT = 0;
        }
        break;
    }
    case F_RESOLVE:
        if (fOk) { rdBrow(r, 0, false); rdEyes(r, 0, false, 0); rdMouth(r, fVuln ? 2 : 0, false); }
        else if (fPT & 1)                        // hit: whole face flashes
            for (int y = 0; y < rdFaceH; y++) for (int x = 0; x < rdW; x++) r.setPixel(x, y, 255);
        if (fPT >= 8) { fPhase = F_GAP; fPT = 0; fGapT = 40 + esp_random() % 40; }
        break;
    case F_WIN:                                  // the melt, then back to showcase
        for (int x = 0; x < rdW && x < 16; x++)
            if ((esp_random() % 5) == 0 && rdMelt[x] < rdH) rdMelt[x]++;
        for (int x = 5; x <= 10; x++) { int cy = 12 + rdMelt[x]; if (cy < rdH) r.setPixel(x, cy, 200); }
        for (int e = 0; e < 2; e++) { int bx = e ? 10 : 3;
            for (int y = 3; y <= 5; y++) for (int x = 0; x < 3; x++) {
                int cy = y + rdMelt[bx + x];
                if (cy < rdH && !(y == 4 && x == 1)) r.setPixel(bx + x, cy, 200); } }
        if (fPT > 80) { fOn = false; rdReset(0); }
        return;                                  // no bars over the death
    case F_LOSE:
        if (fPT < 30) { rdMouth(r, 0, true); rdBrow(r, 0, true); rdEyes(r, 0, false, (fPT & 4) ? 1 : -1); rdNoise(r, 8, rdFaceH); }
        if (fPT > 40) { fOn = false; rdReset(0); }
        return;
    }
    fightDrawBars(r);
}

bool raidTick(Renderer& r, uint32_t now) {
    if (now - rdLast < RD_TICKMS) return false;
    rdLast = now;
    rdT++;
    if (fOn) { r.clear(); fightTick(r); return true; }
    if (rdAuto && rdT > 85) rdReset(rdAnimIdx + 1);

    r.clear();
    switch (rdBoss) {
        case VANTA:   tickVanta(r);   break;
        case MOTH:    tickMoth(r);    break;
        case CHORUS:  tickChorus(r);  break;
        case BULWARK: tickBulwark(r); break;
        case NULLK:   tickNull(r);    break;
    }
    return true;
}
