#include "raid.h"
#include "renderer.h"
#include <Arduino.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Raid 16 — the M0.5 boss showcase (five parametric face anatomies with their
// signature animations) plus the M1 fight engine below it (weighted-deck
// telegraphs, phases, moods, lobby, win/lose/stats — see the block comment at
// the engine). No sprite sheets: faces are drawn from state each tick.
// Brightness discipline: dim 30–70 / body 200 / highlight 255.
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

static void rdReset(uint8_t idx) {
    rdAnimIdx = idx % ANIM_N[rdBoss];
    rdAnim = ANIM_SET[rdBoss][rdAnimIdx];
    rdT = 0;
    for (int i = 0; i < 6; i++) { rdPY[i] = -(float)(i * 5); rdPX[i] = 0; }
    memset(rdMelt, 0, sizeof(rdMelt));
}

static void rdSetBoss(uint8_t b) { rdBoss = b % NBOSS; rdReset(0); }

static void fightReset();   // fwd — back to the showcase when the game restarts

void raidInit(Renderer& r) {
    rdW = r.width(); rdH = r.height();
    rdFaceH = rdH < 16 ? rdH : 16;
    rdStageY = rdH >= 24 ? 16 : -1;
    rdAuto = true; rdLast = 0;
    fightReset();
    rdSetBoss(VANTA);
}

static bool fightInput(uint8_t p, char k);   // fwd (fight engine sits below the showcase)
static bool fightActive();                   // fwd — true whenever the fight flow owns the screen

void raidInput(uint8_t p, char k, bool pressed) {
    if (!pressed) return;
    if (k == 'G' || fightActive()) { fightInput(p, k); return; }
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
        if (lift < 0) lift = 0;
        if (lift > 8) lift = 8;
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
                    if (x < c || x >= rdW - c || (int)(esp_random() % 10) < c)
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



// ====================== M1+M2: the fight engine ============================
// docs/raid16.md §11. M1 shipped the VANTA fight: weighted deck-draw
// telegraphs with pity rules, phases, moods, windows/hull/vuln, lobby and
// stats, with decks driven by the ~10 Hz raidNet() snapshot. M2 makes the
// engine data-driven per §10 — bosses are const tables + anatomy programs,
// not code forks — and adds:
//   · the SIM LEVEL knob D1-D4 (window/cadence/hull/enrage scalars plus
//     mechanic gating: DRILL is pure alphabet practice, NIGHTMARE forges duds)
//   · MOTH (feints that punish a committed shield, hacker pulse readout,
//     DUST that blinds the decks' lane alerts until wiped or shot clear)
//   · BULWARK (armored 1-damage cap, the ping→riposte→visor-lift BAIT loop,
//     ping/heavy forge fork, BASH deck-shudder, MOCKING fake lifts)
//   · disconnect-pause (claimed decks heartbeat 'H'; a silent deck >10 s
//     pauses the fight with the boss visibly asleep until it returns)
//   · the assist governor (two consecutive wipes at D1-D2: +2 hull, -10%
//     cadence, announced as ASSISTED on the nameplate)
// Party scaling: size 1 = solo-sim rules (open blink code, no shell pipeline,
// no acid/jam/dust/lanes); 2-4 turns the full deck layers on. Deck bundling
// for 2-3p lives client-side (raid.js); the device only needs role numbers.
//
// Fight keys: L/R rocker · 1-4 dial (lobby: party) · O overcharge · K crank
// · F fire heavy · P fire ping (Bulwark) · T send heavy/generic shell ·
// U send ping · W wipe stroke · a-d resync pad · X dodge · H heartbeat ·
// G advance · Q back. Lobby only: V/M/B pick the boss, D cycles difficulty.

#define SEC(s) ((uint16_t)((s) * 1000 / RD_TICKMS))

enum FState : uint8_t { ST_IDLE, ST_LOBBY, ST_INTRO, ST_FIGHT, ST_WIN, ST_LOSE, ST_STATS };
enum FPhase : uint8_t { F_GAP, F_TELE, F_RESOLVE, F_TAUNT };
enum Tele   : uint8_t { T_SWEEP_L, T_SWEEP_R, T_BEAM, T_CHARGE, T_ACID, T_JAM,
                        T_DUST, T_BASH, T_N,
                        T_RIP = T_N };             // reactive — provoked, never drawn
enum Role   : int8_t  { R_SHIELD = 0, R_GUNNER = 1, R_HACKER = 2, R_MEDIC = 3 };

// Mood (§7.2): rolled per fight, announced on the nameplate, readable in the
// idle stance. dW skews the telegraph deck per type; win/cad scale every
// window / the attack cadence; sigPct drives the boss's signature roll
// (MOTH: feint %, BULWARK: fake-lift %).
struct MoodDef {
    const char* name;
    int8_t  dW[T_N];
    float   winScale, cadScale;
    uint8_t sigPct;
};

// §10: a boss is a const table + one anatomy program. The telegraph engine,
// window resolution, hull, phases and stats are fully shared.
struct FBossDef {
    uint8_t     anatomy;                 // Boss enum — selects the face program
    const char* name;
    int16_t     hp;
    uint8_t     dmg[3];                  // hull cost on miss: sweep / beam / charge
    uint8_t     gapS[3][2];              // cadence range (s) per phase, FIELD 4p
    uint8_t     teleW[3][T_N];           // draw weights per phase
    MoodDef     moods[3];
    bool        armored;                 // BULWARK: damage capped at 1, visor down
};

static const FBossDef FBOSSES[] = {
    { VANTA, "VANTA", 100, { 1, 2, 3 },
      { { 8, 10 }, { 6, 8 }, { 4, 6 } },
      { //swL swR bm chg ac jm du ba
        {  3,  3, 3,  2, 0, 0, 0, 0 },
        {  2,  2, 3,  2, 2, 2, 0, 0 },
        {  3,  3, 3,  3, 1, 1, 0, 0 } },
      { { "COLD",    { 0 },                        1.00f, 1.0f, 0 },
        { "CURIOUS", { 0, 0, 2, 0, 0, 0, 0, 0 },  1.15f, 1.0f, 0 },
        { "STERN",   { 2, 2, 0, 0, 0, 0, 0, 0 },  0.85f, 1.0f, 0 } },
      false },
    { MOTH, "MOTH", 85, { 1, 1, 2 },               // weaker hits, faster cadence
      { { 6, 8 }, { 5, 7 }, { 4, 5 } },
      { {  3,  3, 3,  1, 0, 0, 2, 0 },
        {  2,  2, 3,  2, 1, 1, 2, 0 },
        {  3,  3, 3,  2, 1, 1, 1, 0 } },
      { { "SKITTISH", { 0 },                       1.00f, 1.0f, 45 },
        { "FRENZIED", { 0 },                       1.00f, 1.2f, 30 },
        { "SLY",      { 0, 0, 1, 0, 0, 0, 0, 0 }, 0.90f, 1.0f, 35 } },
      false },
    { BULWARK, "BULWARK", 70, { 1, 2, 3 },
      { { 8, 10 }, { 6, 8 }, { 5, 7 } },
      { {  3,  3, 1,  2, 0, 0, 0, 2 },
        {  2,  2, 1,  2, 1, 1, 0, 3 },
        {  3,  3, 1,  3, 1, 1, 0, 2 } },
      { { "PATIENT",  { 0 },                          1.00f, 1.0f, 0 },
        { "WRATHFUL", { 0, 0, 0, 1, 0, 0, 0, 2 },    0.90f, 1.1f, 0 },
        { "MOCKING",  { 0 },                          1.00f, 1.0f, 35 } },
      true },
};
static const uint8_t N_FBOSS = sizeof(FBOSSES) / sizeof(FBOSSES[0]);

// The SIM LEVEL knob (§6), D1-D4. D5 SIGNAL-LOST ships with M4. enrageS 0 =
// enrage off. Gates: DRILL drops every signature/afflication for pure
// alphabet practice; NIGHTMARE's sloppy forges make duds. (M2 simplification:
// the dud is a flat 15% roll at send time — forge sloppiness isn't measured —
// and D4's control remap on bash stays cosmetic client-side.)
struct DiffDef { const char* name; float win, cad; int8_t hull; uint8_t enrageS;
                 bool feints, afflict, duds; };
static const DiffDef DIFFS[4] = {
    { "DRILL",     1.50f, 0.70f, 12,  0, false, false, false },
    { "FIELD",     1.00f, 1.00f, 10, 90, true,  true,  false },
    { "VETERAN",   0.85f, 1.15f, 10, 90, true,  true,  false },
    { "NIGHTMARE", 0.70f, 1.30f,  8, 75, true,  true,  true  },
};

static const float PARTY_CAD[5] = { 1.0f, 0.5f, 0.65f, 0.8f, 1.0f };

static FState   fState = ST_IDLE;
static uint8_t  fBoss  = 0;                        // index into FBOSSES
static uint8_t  fDiff  = 1;                        // FIELD
static uint8_t  fParty = 1;
static uint8_t  fMood;
static FPhase   fPhase; static uint8_t fPhaseNo;   // fight sub-state, phase 1..3
static uint8_t  fType;                             // active telegraph (may be T_RIP)
static uint8_t  fFreq, fGlyph;                     // beam: frequency + hacker glyph
static uint8_t  fCode[4];                          // codebook: glyph -> frequency 1..4
static uint8_t  fRipSide;                          // riposte: side to block (1 L / 2 R)
static int16_t  fHP;  static int8_t fHull;
static uint16_t fPT, fGapT, fWin;
static uint8_t  fSide, fDial; static bool fOver, fOk;
static uint8_t  fCrank, fShells;                   // generic breach (VANTA/MOTH)
static uint8_t  fPing, fHeavy;                     // BULWARK's split breach
static bool     fDudGen, fDudPing, fDudHeavy;      // D4 sloppy-forge duds
static uint16_t fVuln; static int16_t fEnrage;     // enrage <0 = not armed
static int16_t  fVisor; static bool fFakeLift;     // BULWARK: lift ticks left
static uint16_t fBash;                             // BULWARK: shudder ticks left
static bool     fFeint; static uint8_t fFeintFx;   // MOTH: current tele is fake
static uint16_t fShlCd;                            // shield cooldown (feint punish)
static uint8_t  fDust;                             // MOTH: wipe strokes to clear
static uint8_t  fFxShot;
static int8_t   fAcid; static uint8_t fAcidHP;
static int8_t   fJam;  static uint8_t fResync;
static int8_t   fLane; static uint16_t fLaneT, fLaneGap;
static uint8_t  fLast1, fLast2;                    // pity: last two draws
static uint16_t fSinceBeam;                        // pity: forced window chance
static uint32_t fEv; static char fEvName[12];

// Disconnect-pause (§4): decks claim their roles in the lobby (any input or
// 'H' heartbeat), and a claimed role silent >10 s pauses the fight.
static uint32_t fSeenMs[4]; static bool fClaimed[4];
static int8_t   fPausedRole = -1;

// Assist governor (§6): consecutive wipes per boss; applied at D1-D2.
static uint8_t  fWipeStreak[N_FBOSS];
static bool     fAssist;

static struct {
    uint16_t t;
    uint8_t  blk, miss, itr, wip, fix, shots, hullLost;
    uint16_t dmg, vdmg;
    const char* res;
} fStats;

#define FB (FBOSSES[fBoss])
#define MD (FB.moods[fMood])
#define DD (DIFFS[fDiff])

static bool fightActive() { return fState != ST_IDLE; }
static void fightReset()  { fState = ST_IDLE; }

// One-shot deck feedback. Callers MUST emit the incidental event (hit/bosshit/
// lanehit) BEFORE calling bossDamage()/hullHit(), because those may in turn
// emit a phase/win/wipe event that has to be the one the decks actually see —
// emitting afterwards silently clobbered every "win", "wipe" and "phase".
static void fEvent(const char* n) {
    fEv++;
    strncpy(fEvName, n, sizeof(fEvName) - 1);
    fEvName[sizeof(fEvName) - 1] = 0;
}

// Max hull for this fight (difficulty baseline + the assist governor's bonus).
// The 8-segment stage bar and the deck's LED row both scale to it.
static int hullMax() { return DIFFS[fDiff].hull + (fAssist ? 2 : 0); }

static bool roleDown(int8_t role) { return fAcid == role || fJam == role; }

static uint16_t rollGap() {
    uint16_t lo = SEC(FB.gapS[fPhaseNo - 1][0]), hi = SEC(FB.gapS[fPhaseNo - 1][1]);
    uint16_t g = lo + esp_random() % (uint16_t)(hi - lo + 1);
    float scale = PARTY_CAD[fParty] / MD.cadScale / DD.cad * (fAssist ? 1.1f : 1.0f);
    return (uint16_t)(g * scale);
}
static uint16_t rollWin(uint8_t t) {
    uint16_t base;
    if      (t == T_BEAM)   base = SEC(4);
    else if (t == T_CHARGE) base = SEC(3);
    else if (t == T_RIP)    base = SEC(1.8f);      // the riposte is FAST (§3.4)
    else if (t == T_ACID || t == T_JAM || t == T_DUST) base = SEC(1.5f);   // land anim
    else if (t == T_BASH)   base = SEC(1.2f);
    else                    base = SEC(2.5f);      // sweeps
    return (uint16_t)(base * MD.winScale * DD.win);
}
static uint16_t rollLaneGap() { return SEC(4) + esp_random() % SEC(2); }

// BULWARK's armor: everything is capped at 1 while the visor is down — and a
// MOCKING fake lift never uncaps, that's the bait. Chip damage can't win; the
// riposte loop is the strategy.
static int capA(int d) {
    return (FB.armored && (fVisor <= 0 || fFakeLift) && d > 1) ? 1 : d;
}

// Deck-draw with pity rules (§7.1). Mood deltas skew the deck; DRILL gates
// out afflictions; solo never draws them (§4); one acid/jam at a time.
static uint8_t drawTele() {
    if (fSinceBeam > SEC(45)) return T_BEAM;
    uint8_t w[T_N]; uint16_t tot = 0;
    for (uint8_t t = 0; t < T_N; t++) {
        int wt = FB.teleW[fPhaseNo - 1][t] + MD.dW[t];
        if (t == T_ACID || t == T_JAM || t == T_DUST)
            if (fParty < 2 || !DD.afflict) wt = 0;
        if (t == T_ACID && fAcid >= 0)  wt = 0;
        if (t == T_JAM  && fJam  >= 0)  wt = 0;
        if (t == T_DUST && fDust > 0)   wt = 0;
        if (t == fLast1 && t == fLast2) wt = 0;    // pity: no 3x running
        w[t] = wt < 0 ? 0 : (uint8_t)wt;
        tot += w[t];
    }
    if (!tot) return T_BEAM;
    uint16_t roll = esp_random() % tot;
    for (uint8_t t = 0; t < T_N; t++) { if (roll < w[t]) return t; roll -= w[t]; }
    return T_BEAM;
}

static void toWin() {
    fState = ST_WIN; fPT = 0; fStats.res = "win";
    fWipeStreak[fBoss] = 0;
    rdBoss = FB.anatomy; rdAuto = false;
    rdReset(ANIM_N[rdBoss] - 1);                   // the boss's own death program
    fEvent("win");
}
static void toLose() {
    fState = ST_LOSE; fPT = 0; fStats.res = "wipe";
    if (fWipeStreak[fBoss] < 250) fWipeStreak[fBoss]++;
    fEvent("wipe");
}

static void checkPhase() {
    uint8_t want = fHP > FB.hp * 2 / 3 ? 1 : (fHP > FB.hp / 3 ? 2 : 3);
    if (want > fPhaseNo) {
        fPhaseNo = want;
        fPhase = F_TAUNT; fPT = 0;
        if (fPhaseNo == 3) {
            if (DD.enrageS) fEnrage = SEC(DD.enrageS);
            // Lane fire is an affliction, so DRILL strips it like acid/jam/dust.
            // Gated explicitly: it used to ride on DD.enrageS, which left
            // fLaneGap at whatever the PREVIOUS fight had rolled — so whether
            // DRILL got lane fire depended on leftover state, not difficulty.
            if (DD.afflict) fLaneGap = rollLaneGap();
        }
        fEvent("phase");
    }
}

static void bossDamage(int d) {
    fHP -= d;
    fStats.dmg += d;
    if (fHP <= 0) { fHP = 0; toWin(); return; }
    checkPhase();
}

static void hullHit(int8_t n) {
    fHull -= n;
    fStats.hullLost += n;
    if (fHull <= 0) { fHull = 0; toLose(); }
}

static void clearDust(const char* how) {
    if (fDust) { fDust = 0; fEvent(how); }
}

// Heavy / generic shot ('F'): crank to 100, and at 2p+ the breach must hold a
// shell. Interrupts a charge; ×3 in a vuln window; ×3 through an open visor.
static void fireShot() {
    if (fCrank < 100) return;
    bool dud = false;
    if (fParty >= 2) {
        if (FB.armored) {
            if (!fHeavy) { fEvent("noshell"); return; }
            fHeavy = 0; dud = fDudHeavy; fDudHeavy = false;
        } else {
            if (!fShells) { fEvent("noshell"); return; }
            fShells--; dud = fDudGen; fDudGen = false;
        }
    }
    fCrank = 0; fFxShot = 4; fStats.shots++;
    if (dud) { fEvent("dud"); return; }
    clearDust("dustshot");                          // a live shot clears MOTH's motes
    if (fPhase == F_TELE && fType == T_CHARGE) {    // charge interrupt!
        fStats.itr++; fOk = true; fPhase = F_RESOLVE; fPT = 0;
        fEvent("interrupt");
        bossDamage(capA(4));                       // may supersede with phase/win
        return;
    }
    if (fVisor > 0 && !fFakeLift) {                // visor open: uncapped, ×3
        fEvent("vulnhit"); fStats.vdmg += 9; bossDamage(9);
    } else if (fFakeLift && fVisor > 0) {          // MOCKING bait: still armored
        fEvent("baited"); bossDamage(1);
    } else if (fVuln) {
        int d = capA(9);
        fEvent("vulnhit"); fStats.vdmg += d; bossDamage(d);
    } else {
        fEvent("hit"); bossDamage(capA(3));
    }
}

// Ping ('P', BULWARK only): the deliberate light provoke — fired into a quiet
// boss it triggers the riposte counter-telegraph; block that cleanly and the
// visor lifts. No crank needed (it's a light shell); 2p+ needs a forged ping.
static void firePing() {
    if (!FB.armored) return;
    bool dud = false;
    if (fParty >= 2) {
        if (!fPing) { fEvent("noshell"); return; }
        fPing = 0; dud = fDudPing; fDudPing = false;
    }
    fStats.shots++; fFxShot = 2;
    if (dud) { fEvent("dud"); return; }
    clearDust("dustshot");
    if (fPhase == F_GAP && fVisor <= 0) {           // provoke!
        fType = T_RIP;
        fRipSide = (esp_random() & 1) ? 1 : 2;
        fWin = rollWin(T_RIP);
        fSide = 0; fOver = false; fFeint = false;
        fPhase = F_TELE; fPT = 0;
        fEvent("riposte");
        return;
    }
    fEvent("hit"); bossDamage(capA(1));
}

static void fightBegin() {
    fMood = esp_random() % 3;
    for (uint8_t i = 0; i < 4; i++) fCode[i] = i + 1;    // codebook: shuffle 1..4
    for (int8_t i = 3; i > 0; i--) {
        uint8_t j = esp_random() % (i + 1);
        uint8_t t = fCode[i]; fCode[i] = fCode[j]; fCode[j] = t;
    }
    fAssist = fDiff <= 1 && fWipeStreak[fBoss] >= 2;     // the governor (§6)
    fHP = FB.hp; fHull = DD.hull + (fAssist ? 2 : 0);
    fPhaseNo = 1; fPhase = F_GAP;
    fSide = 0; fDial = 0; fOver = false; fOk = false;
    fCrank = 0; fShells = 0; fPing = 0; fHeavy = 0;
    fDudGen = fDudPing = fDudHeavy = false;
    if (fParty >= 2) { if (FB.armored) fPing = 1; else fShells = 1; }   // one pre-load
    fVuln = 0; fEnrage = -1; fVisor = 0; fFakeLift = false;
    fBash = 0; fFeint = false; fFeintFx = 0; fShlCd = 0; fDust = 0;
    fFxShot = 0;
    fAcid = -1; fAcidHP = 0; fJam = -1;
    fLane = -1; fLaneT = 0; fLaneGap = 0;   // must reset: these are file-static and
                                            // a leftover gap would fire lanes in a
                                            // difficulty that has them disabled
    fLast1 = fLast2 = T_N; fSinceBeam = 0;
    memset(&fStats, 0, sizeof(fStats)); fStats.res = "";
    memset(rdMelt, 0, sizeof(rdMelt));
    rdBoss = FB.anatomy; rdAuto = false; rdReset(0);
    uint32_t nowMs = millis();
    for (uint8_t i = 0; i < 4; i++) fSeenMs[i] = nowMs;  // fresh pause grace
    fPausedRole = -1;
    fState = ST_INTRO; fPT = 0;
    fEvent("intro");
}

static void enterLobby() {
    fState = ST_LOBBY;
    for (uint8_t i = 0; i < 4; i++) { fClaimed[i] = false; fSeenMs[i] = millis(); }
    fEvent("lobby");
}

static bool fightInput(uint8_t p, char k) {
    uint8_t role = p < 4 ? p : 0;
    fSeenMs[role] = millis();                      // any traffic proves the deck lives
    if (fState == ST_LOBBY) fClaimed[role] = true; // lobby presence claims the role
    if (k == 'H') return true;                     // heartbeat — liveness only

    switch (fState) {
    case ST_IDLE:
        if (k == 'G') { enterLobby(); return true; }
        return false;
    case ST_LOBBY:
        if (k >= '1' && k <= '4') { fParty = k - '0'; return true; }
        if (k == 'V') { fBoss = 0; return true; }
        if (k == 'M') { fBoss = 1; return true; }
        if (k == 'B') { fBoss = 2; return true; }
        if (k == 'D') { fDiff = (fDiff + 1) % 4; return true; }
        if (k == 'G') { fightBegin(); return true; }
        if (k == 'Q') { fState = ST_IDLE; rdAuto = true; rdSetBoss(VANTA); return true; }
        return true;
    case ST_INTRO:
        if (k == 'Q') { fState = ST_LOBBY; return true; }
        return true;
    case ST_STATS:
        if (k == 'G') { fightBegin(); return true; }
        if (k == 'Q') { fState = ST_LOBBY; return true; }
        return true;
    case ST_WIN: case ST_LOSE:
        return true;
    case ST_FIGHT:
        switch (k) {
        case 'Q': fState = ST_LOBBY; fEvent("abandon"); return true;
        case 'L': if (!roleDown(R_SHIELD) && !fShlCd) fSide = 1; return true;
        case 'R': if (!roleDown(R_SHIELD) && !fShlCd) fSide = 2; return true;
        case '1': case '2': case '3': case '4':
            if (!roleDown(R_SHIELD)) fDial = k - '0';
            return true;
        case 'O': if (!roleDown(R_SHIELD) && !fShlCd) fOver = true; return true;
        case 'K':
            if (!roleDown(R_GUNNER)) fCrank = fCrank > 90 ? 100 : fCrank + 10;
            return true;
        case 'F': if (!roleDown(R_GUNNER)) fireShot(); return true;
        case 'P': if (!roleDown(R_GUNNER)) firePing(); return true;
        case 'T':                                  // medic sends heavy / generic
            if (fParty >= 2 && !roleDown(R_MEDIC)) {
                bool dud = DD.duds && (esp_random() % 100) < 15;
                if (FB.armored) { if (!fHeavy) { fHeavy = 1; fDudHeavy = dud; fEvent("shell"); } }
                else if (fShells < 2) { fShells++; if (dud) fDudGen = true; fEvent("shell"); }
            }
            return true;
        case 'U':                                  // medic sends a ping (BULWARK)
            if (fParty >= 2 && !roleDown(R_MEDIC) && FB.armored && !fPing) {
                fPing = 1; fDudPing = DD.duds && (esp_random() % 100) < 15;
                fEvent("shell");
            }
            return true;
        case 'W':                                  // wipe: acid first, then dust
            if (fAcid >= 0 && fAcidHP > 0) {
                if (--fAcidHP == 0) { fAcid = -1; fStats.wip++; fEvent("wiped"); }
            } else if (fDust > 0) {
                if (--fDust == 0) fEvent("dustclear");
            }
            return true;
        case 'a': case 'b': case 'c': case 'd':
            if (fJam >= 0) {
                if ((uint8_t)(k - 'a') == fResync) { fJam = -1; fStats.fix++; fEvent("resync"); }
                else { fResync = esp_random() % 4; fEvent("rsyfail"); }
            }
            return true;
        case 'X':
            if (fLane >= 0 && (int8_t)role == fLane) {
                fLane = -1; fLaneGap = rollLaneGap(); fEvent("dodge");
            }
            return true;
        }
        return true;
    }
    return true;
}

// --- anatomy rendering helpers (the §10 face programs, fight edition) ------

// BULWARK's slats + slits, drawable with a bash shove.
static void bulwarkSlats(Renderer& r, int shove, bool slitsLit) {
    for (int y = 0; y < rdFaceH; y++)
        if ((y % 3) != 2)
            for (int x = 1; x <= 14; x++) r.setPixel(x + shove, y, 90);
    for (int x = 3; x <= 5; x++)  r.setPixel(x + shove, 4, slitsLit ? 255 : 90);
    for (int x = 10; x <= 12; x++) r.setPixel(x + shove, 4, slitsLit ? 255 : 90);
}

// MOTH's compound eyes forced shut (beam blink code).
static void mothEyesClosed(Renderer& r) {
    for (int e = 0; e < 2; e++) {
        int bx = e ? 10 : 2;
        for (int cy = 0; cy < 4; cy++)
            for (int cx = 0; cx < 4; cx++) r.setPixel(bx + cx, 2 + cy, 0);
        for (int cx = 0; cx < 4; cx++) r.setPixel(bx + cx, 4, 200);
    }
}

// The idle stance per anatomy (gap between telegraphs, lobby, pause).
static void faceIdle(Renderer& r, bool asleep) {
    switch (FB.anatomy) {
    case MOTH:
        mothFace(r, asleep ? 0 : 2, false);
        if (asleep) mothEyesClosed(r);
        break;
    case BULWARK: {
        int shove = fBash ? (((rdT % 4) < 2) ? 1 : -1) : 0;
        if (fVisor > 0) {                          // lifted (or fake-lifted…)
            rdBrow(r, 0, false); rdEyes(r, 0, false, 0); rdMouth(r, 1, false);
            int open = fFakeLift ? SEC(1) : SEC(4);
            int lift = (open - fVisor) * 4;        // rises fast…
            int drop = fVisor * 3;                 // …slams at the end
            int rows = lift < drop ? lift : drop;
            if (rows > 16) rows = 16;
            if (rows < 0)  rows = 0;
            for (int y = rdFaceH - 1; y >= rows; y--)   // remaining slats below the lift
                if (((y - rows) % 3) != 2)
                    for (int x = 1; x <= 14; x++) r.setPixel(x, y, 90);
            if (!fFakeLift && (rdT & 2)) { r.setPixel(0, 12, 255); r.setPixel(15, 12, 255); }
        } else {
            bulwarkSlats(r, shove, !asleep);
            int kn = rdT % 60;                     // it knocks from inside
            if (!asleep && (kn == 40 || kn == 44)) { r.setPixel(7 + shove, 8, 255); r.setPixel(8 + shove, 8, 255); }
        }
        break;
    }
    default: {                                     // VANTA (standard anatomy)
        int ex = fMood == 1 ? ((rdT / 12) % 3) - 1 : 0;   // curious eyes wander
        rdBrow(r, ex, fMood == 2); rdEyes(r, ex, asleep || (fPT % 40) < 2, ex);
        rdMouth(r, fVuln > 0 ? 2 : 0, false);
        break;
    }
    }
}

// One telegraph frame per type × anatomy. The alphabet is shared (§2); each
// anatomy renders it in its own face language.
static void faceTele(Renderer& r) {
    if (fType <= T_SWEEP_R || fType == T_RIP) {    // sweep / riposte: side + edge
        int dir = (fType == T_SWEEP_L || (fType == T_RIP && fRipSide == 1)) ? -1 : 1;
        switch (FB.anatomy) {
        case MOTH:    mothFace(r, 1, true); break;
        case BULWARK: bulwarkSlats(r, 0, true);
                      if (fType == T_RIP)          // riposte: the whole slit row flares
                          for (int x = 2; x <= 13; x++) r.setPixel(x, 4, (fPT & 1) ? 255 : 90);
                      break;
        default:      rdBrow(r, dir * 2, true); rdEyes(r, dir * 2, false, dir);
                      rdMouth(r, 0, false); break;
        }
        int rate = fType == T_RIP ? 0 : 1;         // riposte ripples every tick
        if (rate == 0 || (fPT & 1)) {
            int e = dir < 0 ? 0 : rdW - 1;
            for (int y = 0; y < rdFaceH; y++) r.setPixel(e, y, 255);
        }
    } else if (fType == T_BEAM) {                  // blink code = the frequency
        int cyc = fFreq * 8 + 14, ph = fPT % cyc;
        bool closed = ph < fFreq * 8 && (ph % 8) < 4;
        switch (FB.anatomy) {
        case MOTH:    mothFace(r, 1, true); if (closed) mothEyesClosed(r); break;
        case BULWARK: bulwarkSlats(r, 0, !closed); break;
        default:      rdBrow(r, 0, true); rdEyes(r, 0, closed, 0);
                      rdMouth(r, 0, false); break;
        }
    } else if (fType == T_CHARGE) {                // building blast
        int stage = 1 + fPT * 3 / fWin;
        switch (FB.anatomy) {
        case MOTH: {
            mothFace(r, 2, true);
            int rr = stage;                        // glow grows between the mandibles
            for (int dy = -rr; dy <= rr; dy++)
                for (int dx = -rr; dx <= rr; dx++)
                    if (dx * dx + dy * dy <= rr * rr) r.setPixel(8 + dx, 12 + dy, 90 + stage * 40);
            break;
        }
        case BULWARK: {
            bulwarkSlats(r, 0, true);
            int rr = stage;                        // it glows through the slats
            for (int dy = -rr; dy <= rr; dy++)
                for (int dx = -rr; dx <= rr; dx++)
                    if (dx * dx + dy * dy <= rr * rr) r.setPixel(8 + dx, 8 + dy, 200);
            break;
        }
        default: rdBrow(r, 0, true); rdEyes(r, 0, false, 0); rdMouth(r, stage, false); break;
        }
    } else if (fType == T_ACID) {                  // glops fall out of the face
        faceIdle(r, false);
        for (int i = 0; i < 3; i++) {
            rdPY[i] += 0.8f;
            if (rdPY[i] > rdH - 1) { rdPY[i] = 13; rdPX[i] = 6 + esp_random() % 4; }
            if (rdPY[i] >= 13 && rdPY[i] <= rdH - 2)
                r.setPixel(rdPX[i] ? rdPX[i] : 7, (int)rdPY[i], 255);
        }
    } else if (fType == T_JAM) {                   // static ramps up
        faceIdle(r, false);
        rdNoise(r, fPT < 40 ? fPT : 40, rdFaceH);
    } else if (fType == T_DUST) {                  // wing flaps scatter motes
        mothFace(r, 3, false);
        for (int i = 0; i < 6; i++) {
            rdPY[i] += 0.3f + (i % 3) * 0.1f;
            if (rdPY[i] > rdH - 1) { rdPY[i] = 6; rdPX[i] = esp_random() % rdW; }
            if (rdPY[i] >= 0) r.setPixel((rdPX[i] + ((rdT + i) & 1)) % rdW, (int)rdPY[i], 45);
        }
    } else if (fType == T_BASH) {                  // winds up, then rams the frame
        int shove = fPT > fWin / 2 ? ((fPT & 2) ? 1 : -1) : 0;
        bulwarkSlats(r, shove, true);
        if (fPT + 2 >= fWin && rdStageY >= 0)
            for (int x = 0; x < rdW; x += 3) r.setPixel(x, rdStageY, 255);
    }
    // MOTH's tell for the hacker: a real telegraph holds steady — a feint's
    // antenna tips flicker hard (the panel-side pulse; the deck mirrors it).
    if (fFeint && (fPT & 1)) { r.setPixel(3, 0, 255); r.setPixel(12, 0, 255); }
}

static void lobbyTick(Renderer& r) {
    faceIdle(r, false);
    for (uint8_t i = 0; i < 4; i++)                       // party pips (left)
        r.setPixel(2 + i * 2, rdH - 2, i < fParty ? 255 : 40);
    for (uint8_t i = 0; i < 4; i++)                       // difficulty pips (right)
        r.setPixel(rdW - 8 + i * 2, rdH - 2, i <= fDiff ? 200 : 40);
}

static void introTick(Renderer& r) {
    fPT++;
    char plate[40];
    snprintf(plate, sizeof(plate), "%s THE %s%s", FB.name, MD.name,
             fAssist ? " ASSISTED" : "");
    int len = (int)strlen(plate);
    int scrollTicks = (rdW + len * 6) / 2 + 1;            // 2 px/tick
    if ((int)fPT < scrollTicks) {
        int x = rdW - fPT * 2;
        for (int i = 0; i < len; i++) r.drawChar(x + i * 6, 4, plate[i], 255);
        return;
    }
    int c = (fPT - scrollTicks) / SEC(1);
    if (c >= 3) {
        fState = ST_FIGHT; fPhase = F_GAP; fPT = 0;
        fGapT = SEC(2);                                   // short opening grace
        fEvent("fight");
        return;
    }
    faceIdle(r, c == 0);                                  // the boss wakes at "2"
    r.drawChar((rdW - 5) / 2, (rdFaceH - 7) / 2, (char)('3' - c), 255);
}

static void fightDrawBars(Renderer& r) {
    if (rdStageY < 0) return;                             // 16x16: decks carry the bars
    int lit = fHP > 0 ? (fHP * rdW + FB.hp - 1) / FB.hp : 0;
    bool hot = (fVuln > 0 || (fVisor > 0 && !fFakeLift)) && (rdT & 2);
    for (int x = 0; x < lit && x < rdW; x++)
        r.setPixel(x, rdStageY, hot ? 255 : 200);
    rdStage(r, (fHull * 8 + hullMax() - 1) / hullMax(), fEnrage >= 0 && fEnrage < SEC(20));
    if (fCrank >= 100 && (rdT & 2)) r.setPixel(rdW - 1, rdH - 1, 255);
}

static void fightTick(Renderer& r) {
    // Disconnect-pause: a claimed deck gone silent puts the boss to sleep.
    uint32_t nowMs = millis();
    fPausedRole = -1;
    for (uint8_t i = 0; i < 4; i++)
        if (fClaimed[i] && nowMs - fSeenMs[i] > 10000) { fPausedRole = (int8_t)i; break; }
    if (fPausedRole >= 0) {
        faceIdle(r, true);                                // visibly asleep, timers held
        fightDrawBars(r);
        return;
    }

    fPT++; fStats.t++;
    if (fVuln) fVuln--;
    if (fVisor > 0 && --fVisor == 0) fFakeLift = false;
    if (fBash) fBash--;
    if (fShlCd) fShlCd--;
    if (fFeintFx) fFeintFx--;
    fSinceBeam++;
    if (fEnrage > 0 && --fEnrage == 0) { toLose(); return; }

    // P3 lane fire (2p+): dodge or lose hull. MOTH's dust blinds the decks'
    // lane alerts (raidNet hides the lane) — the stage markers are the backup,
    // and they're buried under motes. Wipe or shoot the dust away.
    if (fPhaseNo == 3 && fParty >= 2) {
        if (fLane < 0) {
            if (fLaneGap > 0 && --fLaneGap == 0) {
                fLane = esp_random() % 4; fLaneT = SEC(2); fEvent("lane");
            }
        } else if (--fLaneT == 0) {
            fLane = -1; fLaneGap = rollLaneGap();
            fEvent("lanehit"); hullHit(1);       // may supersede with "wipe"
            if (fState != ST_FIGHT) return;
        }
    }

    if (fFxShot) { fFxShot--;
        for (int y = 6; y < rdFaceH; y++) r.setPixel(12, y, 255); }

    switch (fPhase) {
    case F_GAP:
        faceIdle(r, false);
        if (fPT >= fGapT) {
            fType = drawTele();
            fLast2 = fLast1; fLast1 = fType;
            fFeint = false;
            if (fType == T_BEAM) {
                fSinceBeam = 0;
                if (fParty >= 2) { fGlyph = esp_random() % 4; fFreq = fCode[fGlyph]; }
                else             { fFreq = 1 + esp_random() % 4; }
            }
            if ((fType <= T_SWEEP_R || fType == T_BEAM) && DD.feints &&
                MD.sigPct && !FB.armored)                 // MOTH: roll the feint
                fFeint = (esp_random() % 100) < MD.sigPct;
            if (fType == T_ACID)
                for (int i = 0; i < 3; i++) { rdPY[i] = 13; rdPX[i] = 6 + esp_random() % 4; }
            if (fType == T_DUST)
                for (int i = 0; i < 6; i++) { rdPY[i] = 2 + i; rdPX[i] = esp_random() % rdW; }
            fWin  = rollWin(fType);
            fSide = 0; fOver = false;
            fPhase = F_TELE; fPT = 0;
        }
        break;
    case F_TELE:
        // A feint dissolves at ~60% of the window: no resolve, no damage —
        // but a shield that already committed eats a 3 s cooldown (§3.2).
        if (fFeint && fPT >= (uint16_t)(fWin * 3 / 5)) {
            if (fSide || fOver) { fShlCd = SEC(3); fEvent("feintpunish"); }
            else fEvent("feint");
            fFeint = false; fFeintFx = 8;
            fOk = true; fPhase = F_RESOLVE; fPT = 0;
            break;
        }
        faceTele(r);
        if (fPT >= fWin) {
            if (fType == T_ACID) {
                fAcid = esp_random() % 4; fAcidHP = 6; fOk = true; fEvent("acid");
            } else if (fType == T_JAM) {
                fJam = esp_random() % 3; fResync = esp_random() % 4;
                fOk = true; fEvent("jam");
            } else if (fType == T_DUST) {
                fDust = 6; fOk = true; fEvent("dust");
            } else if (fType == T_BASH) {
                fBash = SEC(2); fOk = true; fEvent("bash");
            } else if (fType == T_RIP) {
                fOk = fSide == fRipSide;
                if (fOk) {
                    fStats.blk++;
                    fFakeLift = FB.armored && MD.sigPct &&
                                (esp_random() % 100) < MD.sigPct;   // MOCKING bait
                    fVisor = fFakeLift ? SEC(1) : SEC(4);
                    fEvent(fFakeLift ? "visor" : "visor");          // looks identical — that's the point
                } else { fStats.miss++; fEvent("bosshit"); hullHit(2);
                         if (fState != ST_FIGHT) return; }
            } else {
                if      (fType <= T_SWEEP_R) fOk = fSide == (fType == T_SWEEP_L ? 1 : 2);
                else if (fType == T_BEAM)    fOk = fDial == fFreq;
                else                         fOk = fOver;
                if (fOk) {
                    fStats.blk++;
                    if (fType == T_BEAM) { fVuln = SEC(3); fEvent("vuln"); }
                    else fEvent("block");
                } else {
                    fStats.miss++;
                    fEvent("bosshit");             // hullHit may supersede with "wipe"
                    hullHit(FB.dmg[fType == T_BEAM ? 1 : fType == T_CHARGE ? 2 : 0]);
                    if (fState != ST_FIGHT) return;
                }
            }
            fPhase = F_RESOLVE; fPT = 0;
        }
        break;
    case F_RESOLVE:
        if (fOk) {
            faceIdle(r, false);
            if (fFeintFx)                                  // the tell dissolves into motes
                for (int i = 0; i < 10; i++)
                    r.setPixel(esp_random() % rdW, esp_random() % rdFaceH, 45);
        } else if (fPT & 1)
            for (int y = 0; y < rdFaceH; y++) for (int x = 0; x < rdW; x++) r.setPixel(x, y, 255);
        if (fPT >= 8) { fPhase = F_GAP; fPT = 0; fGapT = rollGap(); }
        break;
    case F_TAUNT:
        switch (FB.anatomy) {
        case MOTH:    mothFace(r, 3, true); rdNoise(r, 5, rdFaceH); break;
        case BULWARK: bulwarkSlats(r, (fPT & 2) ? 1 : 0, true); break;
        default:      rdBrow(r, 0, true); rdEyes(r, 0, false, (fPT & 4) ? 1 : -1);
                      rdMouth(r, 0, true); rdNoise(r, 5, rdFaceH); break;
        }
        if (fPT >= SEC(2)) { fPhase = F_GAP; fPT = 0; fGapT = rollGap(); }
        break;
    }

    // MOTH's dust sits on the stage until cleared, burying the lane markers.
    if (fDust > 0 && rdStageY >= 0)
        for (int i = 0; i < 8; i++) {
            uint16_t h = (uint16_t)((i * 7 + (rdT / 3)) * 2654435u);
            r.setPixel(h % rdW, rdStageY + 1 + (h / 16) % (rdH - rdStageY - 1), 45);
        }

    // P3 lane shot: a projectile descending the target's stage lane.
    if (fLane >= 0 && rdStageY >= 0) {
        int total = SEC(2);
        int y = rdStageY + (int)((uint32_t)(total - fLaneT) * (rdH - 1 - rdStageY) / total);
        r.setPixel(2 + fLane * 4, y, (rdT & 2) ? 255 : 90);
    }
    fightDrawBars(r);
}

static void statsTick(Renderer& r) {
    bool win = fStats.res && strcmp(fStats.res, "win") == 0;
    const char* v = win ? "GG" : "KO";
    int x0 = (rdW - 11) / 2, y0 = (rdFaceH - 7) / 2;
    r.drawChar(x0,     y0, v[0], win ? 255 : 200);
    r.drawChar(x0 + 6, y0, v[1], win ? 255 : 200);
    rdStage(r, (fHull * 8 + hullMax() - 1) / hullMax(), false);
}

// The device→deck snapshot (~10 Hz via main.cpp / gameNetSnapshot). Idempotent
// absolute state; decks filter by role. The lane goes dark while MOTH's dust
// is up — that's the dust mechanic reaching the phones.
size_t raidNet(char* buf, size_t cap) {
    if (fState == ST_IDLE) return 0;
    static const char* SN[] = { "idle", "lobby", "intro", "fight", "win", "lose", "stats" };
    bool beamLive = fState == ST_FIGHT && fPhase == F_TELE && fType == T_BEAM && fParty >= 2;
    int hint = 0;                                          // solo Pilot alert diamond
    if (fState == ST_FIGHT) {
        if (fPhase == F_TELE) {
            if      (fType <= T_SWEEP_R || fType == T_RIP) hint = 1;
            else if (fType == T_BEAM)                      hint = 2;
            else if (fType == T_CHARGE)                    hint = 3;
            else                                           hint = 4;
        } else if (fVuln > 0 || (fVisor > 0 && !fFakeLift)) hint = 3;
    }
    int n = snprintf(buf, cap,
        "{\"type\":\"raid\",\"st\":\"%s\",\"boss\":\"%s\",\"diff\":\"%s\","
        "\"party\":%u,\"mood\":\"%s\",\"assist\":%d,\"ph\":%u,"
        "\"hp\":%d,\"hpMax\":%d,\"hull\":%d,\"hullMax\":%d,\"vulnMs\":%u,\"enrage\":%d,"
        "\"crank\":%u,\"shells\":%u,\"ping\":%u,\"heavy\":%u,"
        "\"glyph\":%d,\"code\":[%u,%u,%u,%u],\"pulse\":%d,"
        "\"acid\":%d,\"acidHp\":%u,\"jam\":%d,\"rsy\":%d,"
        "\"lane\":%d,\"laneUp\":%d,\"laneMs\":%u,\"dust\":%d,\"bash\":%u,\"visor\":%u,\"shlCd\":%u,"
        "\"hint\":%d,\"paused\":%d,\"ev\":%lu,\"evn\":\"%s\"",
        SN[fState], FB.name, DD.name,
        fParty, MD.name, fAssist ? 1 : 0, fPhaseNo,
        (int)fHP, (int)FB.hp, (int)fHull, (int)(DD.hull + (fAssist ? 2 : 0)),
        (unsigned)(fVuln * RD_TICKMS),
        fEnrage < 0 ? -1 : (int)(fEnrage * RD_TICKMS / 1000),
        fCrank, fShells, fPing, fHeavy,
        beamLive ? (int)fGlyph : -1,
        fCode[0], fCode[1], fCode[2], fCode[3],
        (fFeint && fState == ST_FIGHT && fPhase == F_TELE) ? 1 : 0,
        (int)fAcid, fAcidHP, (int)fJam, fJam >= 0 ? (int)fResync : -1,
        // MOTH's dust hides WHICH lane is targeted, but "laneUp" still says a
        // shot is inbound — the deck can offer a dodge that the device only
        // honours from the real target, so the team has to read the buried
        // stage markers. Suppressing the shot entirely made dust an
        // unavoidable hull hit, which the spec never asked for.
        fDust > 0 ? -1 : (int)fLane, fLane >= 0 ? 1 : 0,
        (unsigned)((fLane >= 0 && !fDust) ? fLaneT * RD_TICKMS : 0),
        fDust > 0 ? 1 : 0, (unsigned)(fBash * RD_TICKMS),
        (unsigned)(fVisor > 0 ? fVisor * RD_TICKMS : 0),
        (unsigned)(fShlCd * RD_TICKMS),
        hint, (int)fPausedRole,
        (unsigned long)fEv, fEvName);
    if (n < 0 || (size_t)n >= cap) return 0;
    if (fState >= ST_WIN) {
        int m = snprintf(buf + n, cap - n,
            ",\"stats\":{\"res\":\"%s\",\"sec\":%u,\"blk\":%u,\"miss\":%u,"
            "\"dmg\":%u,\"vdmg\":%u,\"shots\":%u,\"itr\":%u,\"wip\":%u,\"fix\":%u,"
            "\"hullLost\":%u}",
            fStats.res, (unsigned)(fStats.t * RD_TICKMS / 1000),
            fStats.blk, fStats.miss, fStats.dmg, fStats.vdmg,
            fStats.shots, fStats.itr, fStats.wip, fStats.fix, fStats.hullLost);
        if (m < 0 || (size_t)(n + m) >= cap) return 0;
        n += m;
    }
    if ((size_t)(n + 1) >= cap) return 0;
    buf[n++] = '}'; buf[n] = 0;
    return (size_t)n;
}

bool raidTick(Renderer& r, uint32_t now) {
    if (now - rdLast < RD_TICKMS) return false;
    rdLast = now;
    rdT++;

    if (fState != ST_IDLE) {
        r.clear();
        switch (fState) {
        case ST_LOBBY: lobbyTick(r); break;
        case ST_INTRO: introTick(r); break;
        case ST_FIGHT: fightTick(r); break;
        case ST_WIN: {
            // Each boss dies its own death: drive its showcase death program
            // (rdReset in toWin selected it) and hand over before it loops.
            fPT++;
            static const uint8_t CUT[] = { 76, 66, 76, 71, 76 };   // per Boss anatomy
            switch (rdBoss) {
                case MOTH:    tickMoth(r);    break;
                case BULWARK: tickBulwark(r); break;
                default:      tickVanta(r);   break;
            }
            if (fPT > CUT[rdBoss < 5 ? rdBoss : 0]) { fState = ST_STATS; fPT = 0; fEvent("stats"); }
            break;
        }
        case ST_LOSE:
            fPT++;
            if (fPT < 30) {
                switch (FB.anatomy) {
                case MOTH:    mothFace(r, 3, true); rdNoise(r, 8, rdFaceH); break;
                case BULWARK: bulwarkSlats(r, (fPT & 2) ? 1 : 0, true);
                              rdNoise(r, 5, rdFaceH); break;
                default:      rdMouth(r, 0, true); rdBrow(r, 0, true);
                              rdEyes(r, 0, false, (fPT & 4) ? 1 : -1);
                              rdNoise(r, 8, rdFaceH); break;
                }
            }
            if (fPT > 40) { fState = ST_STATS; fPT = 0; fEvent("stats"); }
            break;
        case ST_STATS: statsTick(r); break;
        default: break;
        }
        return true;
    }

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
