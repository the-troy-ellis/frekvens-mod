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

void raidInit(Renderer& r) {
    rdW = r.width(); rdH = r.height();
    rdFaceH = rdH < 16 ? rdH : 16;
    rdStageY = rdH >= 24 ? 16 : -1;
    rdAuto = true; rdLast = 0;
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


// ========================== M1: the VANTA fight ============================
// docs/raid16.md §11 M1 — "one real fight": weighted deck-draw telegraph
// engine with pity rules, VANTA's three phases + moods, response windows,
// hull, vulnerability windows, lobby, and win/lose/stats. The device is
// authoritative (§0.6): decks send single-char keys over the game input
// path, and raidNet() publishes a ~10 Hz idempotent JSON snapshot that each
// deck filters down to what its role should see — phones never mirror the
// telegraphs themselves (§0.4); reading the panel stays the game.
//
// Party scaling (§4, M1 subset): party size 1 keeps the solo-sim rules — no
// decode layer (the boss blinks its frequency openly), no shell pipeline, no
// acid/jam/lane fire. At 2-4 players the beam frequency arrives as a glyph
// the Hacker decodes against a per-fight codebook, shells must be forged and
// sent by the Medic before the Gunner can fire, acid/jam telegraphs disable
// decks until the Medic fixes them, and P3 adds per-deck lane shots to dodge.
// Deck *bundles* for 2-3 players are M2 — at M1 those parties share the four
// role tabs.
//
// Fight keys: L/R shield rocker · 1-4 freq dial (party size in the lobby) ·
// O overcharge · K crank +10% · F fire · T send shell (medic) · W wipe
// stroke · a-d resync pad · X dodge (sender's player number must match the
// targeted lane) · G start/advance · Q quit/back.

#define SEC(s) ((uint16_t)((s) * 1000 / RD_TICKMS))

enum FState : uint8_t { ST_IDLE, ST_LOBBY, ST_INTRO, ST_FIGHT, ST_WIN, ST_LOSE, ST_STATS };
enum FPhase : uint8_t { F_GAP, F_TELE, F_RESOLVE, F_TAUNT };
enum Tele   : uint8_t { T_SWEEP_L, T_SWEEP_R, T_BEAM, T_CHARGE, T_ACID, T_JAM, T_N };
enum Role   : int8_t  { R_SHIELD = 0, R_GUNNER = 1, R_HACKER = 2, R_MEDIC = 3 };

// Mood: rolled at fight start, announced on the intro nameplate, readable in
// the idle stance (§7.2). Weight deltas skew the telegraph deck; winScale
// stretches/compresses every response window.
struct MoodDef { const char* name; uint8_t sweepW, beamW; float winScale; };
static const MoodDef MOODS[3] = {
    { "COLD",    0, 0, 1.00f },   // baseline — the textbook fight
    { "CURIOUS", 0, 2, 1.15f },   // more beams, windows +15%
    { "STERN",   2, 0, 0.85f },   // more sweeps, windows -15%
};

// Telegraph draw weights per phase (deck-draw, §7.1). Acid/jam join in P2.
static const uint8_t TELE_W[3][T_N] = {
    // swL swR beam chg acid jam
    {  3,  3,  3,  2,  0,  0 },
    {  2,  2,  3,  2,  2,  2 },
    {  3,  3,  3,  3,  1,  1 },
};
// Cadence: seconds of idle gap between telegraphs per phase (FIELD 4p
// baseline, §3.1), scaled by party size (§4 auto-normalization).
static const uint8_t GAP_S[3][2]  = { { 8, 10 }, { 6, 8 }, { 4, 6 } };
static const float   PARTY_CAD[5] = { 1.0f, 0.5f, 0.65f, 0.8f, 1.0f };

static FState   fState = ST_IDLE;
static uint8_t  fParty = 1;
static uint8_t  fMood;
static FPhase   fPhase; static uint8_t fPhaseNo;   // fight sub-state, phase 1..3
static uint8_t  fType;                             // active telegraph
static uint8_t  fFreq, fGlyph;                     // beam: frequency + hacker glyph
static uint8_t  fCode[4];                          // codebook: glyph -> frequency 1..4
static int16_t  fHP; static int8_t fHull;
static uint16_t fPT, fGapT, fWin;                  // state ticks, gap length, window
static uint8_t  fSide, fDial; static bool fOver, fOk;
static uint8_t  fCrank, fShells;
static uint16_t fVuln; static int16_t fEnrage;     // enrage <0 = not armed (pre-P3)
static uint8_t  fFxShot;
static int8_t   fAcid; static uint8_t fAcidHP;     // acided role / wipe strokes left
static int8_t   fJam;  static uint8_t fResync;     // jammed role / resync key 0-3
static int8_t   fLane; static uint16_t fLaneT, fLaneGap;   // P3 lane shots
static uint8_t  fLast1, fLast2;                    // pity: last two telegraph draws
static uint16_t fSinceBeam;                        // pity: forced window opportunity
static uint32_t fEv; static char fEvName[12];      // one-shot deck feedback events

static struct {
    uint16_t t;                                    // fight length, ticks
    uint8_t  blk, miss, itr, wip, fix, shots, hullLost;
    uint16_t dmg, vdmg;
    const char* res;                               // "", "win", "wipe"
} fStats;

static bool fightActive() { return fState != ST_IDLE; }

static void fEvent(const char* n) {
    fEv++;
    strncpy(fEvName, n, sizeof(fEvName) - 1);
    fEvName[sizeof(fEvName) - 1] = 0;
}

// A role whose deck is acided or jammed is disabled — its inputs are ignored
// until the Medic fixes it (the indirect damage of §2: a downed Shield can't
// block). The Medic's own wipe/resync keys always work.
static bool roleDown(int8_t role) { return fAcid == role || fJam == role; }

static uint16_t rollGap() {
    uint16_t lo = SEC(GAP_S[fPhaseNo - 1][0]), hi = SEC(GAP_S[fPhaseNo - 1][1]);
    uint16_t g = lo + esp_random() % (uint16_t)(hi - lo + 1);
    return (uint16_t)(g * PARTY_CAD[fParty]);
}
static uint16_t rollWin(uint8_t t) {
    uint16_t base = t == T_BEAM ? SEC(4) : (t == T_CHARGE ? SEC(3) : SEC(2.5f));
    if (t == T_ACID || t == T_JAM) base = SEC(1.5f);   // land animation, not a window
    return (uint16_t)(base * MOODS[fMood].winScale);
}
static uint16_t rollLaneGap() { return SEC(4) + esp_random() % SEC(2); }

// Deck-draw with pity rules (§7.1): weights per phase, skewed by mood; never
// the same telegraph three times running; a beam (the only window opener) is
// guaranteed at least every 45 s. Acid/jam are 2p+ only and one-at-a-time.
static uint8_t drawTele() {
    if (fSinceBeam > SEC(45)) return T_BEAM;
    uint8_t w[T_N]; uint16_t tot = 0;
    for (uint8_t t = 0; t < T_N; t++) {
        w[t] = TELE_W[fPhaseNo - 1][t];
        if (t == T_SWEEP_L || t == T_SWEEP_R) w[t] += MOODS[fMood].sweepW;
        if (t == T_BEAM)                      w[t] += MOODS[fMood].beamW;
        if ((t == T_ACID || t == T_JAM) && fParty < 2) w[t] = 0;
        if (t == T_ACID && fAcid >= 0) w[t] = 0;
        if (t == T_JAM  && fJam  >= 0) w[t] = 0;
        if (t == fLast1 && t == fLast2) w[t] = 0;   // pity: no 3x running
        tot += w[t];
    }
    if (!tot) return T_BEAM;   // unreachable with these tables, but never div-0
    uint16_t roll = esp_random() % tot;
    for (uint8_t t = 0; t < T_N; t++) { if (roll < w[t]) return t; roll -= w[t]; }
    return T_BEAM;
}

static void toWin()  { fState = ST_WIN;  fPT = 0; fStats.res = "win";
                       memset(rdMelt, 0, sizeof(rdMelt)); fEvent("win");  }
static void toLose() { fState = ST_LOSE; fPT = 0; fStats.res = "wipe"; fEvent("wipe"); }

// Phase gates at HP thresholds (66/33) fire a ~2 s taunt interstitial; P3
// entry arms the 90 s enrage timer — chip damage can't beat it (§2), the
// vulnerability windows are the win condition.
static void checkPhase() {
    uint8_t want = fHP > 66 ? 1 : (fHP > 33 ? 2 : 3);
    if (want > fPhaseNo) {
        fPhaseNo = want;
        fPhase = F_TAUNT; fPT = 0;
        if (fPhaseNo == 3) { fEnrage = SEC(90); fLaneGap = rollLaneGap(); }
        fEvent("phase");
    }
}

static void bossDamage(int d, bool vuln) {
    fHP -= d;
    fStats.dmg += d;
    if (vuln) fStats.vdmg += d;
    if (fHP <= 0) { fHP = 0; toWin(); return; }
    checkPhase();
}

static void hullHit(int8_t n) {
    fHull -= n;
    fStats.hullLost += n;
    if (fHull <= 0) { fHull = 0; toLose(); }
}

static void fireShot() {
    if (fCrank < 100) return;
    if (fParty >= 2) {                       // 4p rules: no shell, no shot (§5)
        if (!fShells) { fEvent("noshell"); return; }
        fShells--;
    }
    fCrank = 0; fFxShot = 4; fStats.shots++;
    if (fPhase == F_TELE && fType == T_CHARGE) {   // charge interrupt!
        fStats.itr++; fOk = true; fPhase = F_RESOLVE; fPT = 0;
        bossDamage(4, false); fEvent("interrupt");
        return;
    }
    if (fVuln) { bossDamage(9, true); fEvent("vulnhit"); }
    else       { bossDamage(3, false); fEvent("hit"); }
}

static void fightBegin() {
    fMood = esp_random() % 3;
    for (uint8_t i = 0; i < 4; i++) fCode[i] = i + 1;    // codebook: shuffle 1..4
    for (int8_t i = 3; i > 0; i--) {                     // (reshuffled per fight, §7.5)
        uint8_t j = esp_random() % (i + 1);
        uint8_t t = fCode[i]; fCode[i] = fCode[j]; fCode[j] = t;
    }
    fHP = 100; fHull = 10;                               // FIELD baselines (§6)
    fPhaseNo = 1; fPhase = F_GAP;
    fSide = 0; fDial = 0; fOver = false; fOk = false;
    fCrank = 0; fShells = fParty >= 2 ? 1 : 0;           // one shell pre-loaded for flow
    fVuln = 0; fEnrage = -1; fFxShot = 0;
    fAcid = -1; fAcidHP = 0; fJam = -1; fLane = -1;
    fLast1 = fLast2 = T_N; fSinceBeam = 0;
    memset(&fStats, 0, sizeof(fStats)); fStats.res = "";
    memset(rdMelt, 0, sizeof(rdMelt));
    rdSetBoss(VANTA);                                    // fight rendering reuses VANTA parts
    fState = ST_INTRO; fPT = 0;
    fEvent("intro");
}

// All fight-flow input. Returns true when the key was consumed (everything is,
// once the fight flow owns the screen — stray showcase keys must not leak).
static bool fightInput(uint8_t p, char k) {
    switch (fState) {
    case ST_IDLE:
        if (k == 'G') { fState = ST_LOBBY; fEvent("lobby"); return true; }
        return false;
    case ST_LOBBY:
        if (k >= '1' && k <= '4') { fParty = k - '0'; return true; }
        if (k == 'G') { fightBegin(); return true; }
        if (k == 'Q') { fState = ST_IDLE; rdReset(0); return true; }
        return true;
    case ST_INTRO:
        if (k == 'Q') { fState = ST_LOBBY; return true; }
        return true;
    case ST_STATS:
        if (k == 'G') { fightBegin(); return true; }     // rematch
        if (k == 'Q') { fState = ST_LOBBY; return true; }
        return true;
    case ST_WIN: case ST_LOSE:
        return true;                                     // let the animation finish
    case ST_FIGHT:
        switch (k) {
        case 'Q': fState = ST_LOBBY; fEvent("abandon"); return true;
        case 'L': if (!roleDown(R_SHIELD)) fSide = 1; return true;
        case 'R': if (!roleDown(R_SHIELD)) fSide = 2; return true;
        case '1': case '2': case '3': case '4':
            if (!roleDown(R_SHIELD)) fDial = k - '0';
            return true;
        case 'O': if (!roleDown(R_SHIELD)) fOver = true; return true;
        case 'K':
            if (!roleDown(R_GUNNER)) fCrank = fCrank > 90 ? 100 : fCrank + 10;
            return true;
        case 'F': if (!roleDown(R_GUNNER)) fireShot(); return true;
        case 'T':                                        // medic sends a forged shell
            if (fParty >= 2 && !roleDown(R_MEDIC) && fShells < 2) {
                fShells++; fEvent("shell");
            }
            return true;
        case 'W':                                        // wipe stroke (always allowed)
            if (fAcid >= 0 && fAcidHP > 0 && --fAcidHP == 0) {
                fAcid = -1; fStats.wip++; fEvent("wiped");
            }
            return true;
        case 'a': case 'b': case 'c': case 'd':          // resync pad
            if (fJam >= 0) {
                if ((uint8_t)(k - 'a') == fResync) { fJam = -1; fStats.fix++; fEvent("resync"); }
                else { fResync = esp_random() % 4; fEvent("rsyfail"); }
            }
            return true;
        case 'X':                                        // dodge — must come from the targeted deck
            if (fLane >= 0 && (int8_t)p == fLane) {
                fLane = -1; fLaneGap = rollLaneGap(); fEvent("dodge");
            }
            return true;
        }
        return true;
    }
    return true;
}

// --- per-state panel rendering + advancement (all at RD_TICKMS) ---

static void lobbyTick(Renderer& r) {
    // Vanta idles while the party gathers; party-size pips along the bottom.
    int ex = (rdT / 25) % 3 - 1;
    rdBrow(r, ex, false); rdEyes(r, ex, (rdT % 43) < 2, ex); rdMouth(r, 0, false);
    for (uint8_t i = 0; i < 4; i++)
        r.setPixel(4 + i * 2, rdH - 2, i < fParty ? 255 : 40);
    rdStage(r, 8, false);
}

static void introTick(Renderer& r) {
    // Nameplate scroll ("VANTA THE CURIOUS", §7.2) then a 3-2-1 wake countdown.
    fPT++;
    char plate[24];
    snprintf(plate, sizeof(plate), "VANTA THE %s", MOODS[fMood].name);
    int len = (int)strlen(plate);
    int scrollTicks = (rdW + len * 6) / 2 + 1;           // 2 px/tick
    if ((int)fPT < scrollTicks) {
        int x = rdW - fPT * 2;
        for (int i = 0; i < len; i++) r.drawChar(x + i * 6, 4, plate[i], 255);
        return;
    }
    int c = (fPT - scrollTicks) / SEC(1);
    if (c >= 3) {
        fState = ST_FIGHT; fPhase = F_GAP; fPT = 0;
        fGapT = SEC(2);                                  // short opening grace
        fEvent("fight");
        return;
    }
    rdBrow(r, 0, false); rdEyes(r, 0, c == 0, 0);        // the boss wakes at "2"
    rdMouth(r, 0, false);
    r.drawChar((rdW - 5) / 2, (rdFaceH - 7) / 2, (char)('3' - c), 255);
}

static void fightDrawBars(Renderer& r) {
    if (rdStageY < 0) return;                            // 16x16: decks carry the bars
    int lit = fHP > 0 ? (fHP * rdW + 99) / 100 : 0;      // boss HP across the top stage row
    for (int x = 0; x < lit && x < rdW; x++)
        r.setPixel(x, rdStageY, fVuln > 0 && (rdT & 2) ? 255 : 200);
    rdStage(r, (fHull * 8 + 9) / 10, fEnrage >= 0 && fEnrage < SEC(20));
    if (fCrank >= 100 && (rdT & 2)) r.setPixel(rdW - 1, rdH - 1, 255);   // "gun hot" pip
}

static void fightTick(Renderer& r) {
    fPT++; fStats.t++;
    if (fVuln) fVuln--;
    fSinceBeam++;
    if (fEnrage > 0 && --fEnrage == 0) { toLose(); return; }

    // P3 lane fire (2p+): a lane shot telegraphs on one deck; that deck must
    // dodge before impact. Runs alongside the main telegraph loop.
    if (fPhaseNo == 3 && fParty >= 2 && fState == ST_FIGHT) {
        if (fLane < 0) {
            if (fLaneGap > 0 && --fLaneGap == 0) {
                fLane = esp_random() % 4; fLaneT = SEC(2); fEvent("lane");
            }
        } else if (--fLaneT == 0) {
            fLane = -1; fLaneGap = rollLaneGap();
            hullHit(1); fEvent("lanehit");
            if (fState != ST_FIGHT) return;
        }
    }

    if (fFxShot) { fFxShot--;                            // muzzle flash at the boss
        for (int y = 6; y < rdFaceH; y++) r.setPixel(12, y, 255); }

    switch (fPhase) {
    case F_GAP: {
        int ex = fMood == 1 ? ((rdT / 12) % 3) - 1 : 0;  // curious eyes wander
        rdBrow(r, ex, fMood == 2); rdEyes(r, ex, (fPT % 40) < 2, ex);
        rdMouth(r, fVuln > 0 ? 2 : 0, false);            // vuln: mouth hangs open — shoot it
        if (fPT >= fGapT) {
            fType = drawTele();
            fLast2 = fLast1; fLast1 = fType;
            if (fType == T_BEAM) {
                fSinceBeam = 0;
                if (fParty >= 2) { fGlyph = esp_random() % 4; fFreq = fCode[fGlyph]; }
                else             { fFreq = 1 + esp_random() % 4; }
            }
            if (fType == T_ACID)                          // reuse the spew particle pool
                for (int i = 0; i < 3; i++) { rdPY[i] = 13; rdPX[i] = 6 + esp_random() % 4; }
            fWin  = rollWin(fType);
            fSide = 0; fOver = false;
            fPhase = F_TELE; fPT = 0;
        }
        break;
    }
    case F_TELE: {
        if (fType <= T_SWEEP_R) {                        // sweep: eyes + edge ripple
            int dir = fType == T_SWEEP_L ? -1 : 1;
            rdBrow(r, dir * 2, true); rdEyes(r, dir * 2, false, dir);
            rdMouth(r, 0, false);
            if (fPT & 1) { int e = dir < 0 ? 0 : rdW - 1;
                for (int y = 0; y < rdFaceH; y++) r.setPixel(e, y, 255); }
        } else if (fType == T_BEAM) {                    // beam: blink code = the frequency
            int cyc = fFreq * 8 + 14, ph = fPT % cyc;
            bool closed = ph < fFreq * 8 && (ph % 8) < 4;
            rdBrow(r, 0, true); rdEyes(r, 0, closed, 0); rdMouth(r, 0, false);
        } else if (fType == T_CHARGE) {                  // charge: mouth opens on a timer
            rdBrow(r, 0, true); rdEyes(r, 0, false, 0);
            rdMouth(r, 1 + fPT * 3 / fWin, false);
        } else if (fType == T_ACID) {                    // acid: glops fall from the mouth
            rdBrow(r, 0, true); rdEyes(r, 0, false, 0); rdMouth(r, 2, false);
            for (int i = 0; i < 3; i++) {
                rdPY[i] += 0.8f;
                if (rdPY[i] > rdH - 1) { rdPY[i] = 13; rdPX[i] = 6 + esp_random() % 4; }
                if (rdPY[i] >= 13 && rdPY[i] <= rdH - 2)
                    r.setPixel(rdPX[i] ? rdPX[i] : 7, (int)rdPY[i], 255);
            }
        } else {                                         // jam: static ramps up
            int jx = (esp_random() % 3) - 1;
            rdBrow(r, jx, false); rdEyes(r, jx, false, jx); rdMouth(r, 0, false);
            rdNoise(r, fPT < 40 ? fPT : 40, rdFaceH);
        }
        if (fPT >= fWin) {
            if (fType == T_ACID) {                       // lands on a random deck
                fAcid = esp_random() % 4; fAcidHP = 6; fOk = true; fEvent("acid");
            } else if (fType == T_JAM) {                 // never the medic — they fix it
                fJam = esp_random() % 3; fResync = esp_random() % 4;
                fOk = true; fEvent("jam");
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
                    hullHit(fType == T_BEAM ? 2 : fType == T_CHARGE ? 3 : 1);
                    fEvent("bosshit");
                    if (fState != ST_FIGHT) return;
                }
            }
            fPhase = F_RESOLVE; fPT = 0;
        }
        break;
    }
    case F_RESOLVE:
        if (fOk) { rdBrow(r, 0, false); rdEyes(r, 0, false, 0); rdMouth(r, fVuln ? 2 : 0, false); }
        else if (fPT & 1)                                // hit: whole face flashes
            for (int y = 0; y < rdFaceH; y++) for (int x = 0; x < rdW; x++) r.setPixel(x, y, 255);
        if (fPT >= 8) { fPhase = F_GAP; fPT = 0; fGapT = rollGap(); }
        break;
    case F_TAUNT:                                        // phase-gate interstitial (§9)
        rdBrow(r, 0, true); rdEyes(r, 0, false, (fPT & 4) ? 1 : -1);
        rdMouth(r, 0, true);
        rdNoise(r, 5, rdFaceH);
        if (fPT >= SEC(2)) { fPhase = F_GAP; fPT = 0; fGapT = rollGap(); }
        break;
    }

    // P3 lane shot rendered as a projectile descending the target's stage lane.
    if (fLane >= 0 && rdStageY >= 0) {
        int total = SEC(2);
        int y = rdStageY + (int)((uint32_t)(total - fLaneT) * (rdH - 1 - rdStageY) / total);
        r.setPixel(2 + fLane * 4, y, (rdT & 2) ? 255 : 90);
    }
    fightDrawBars(r);
}

static void statsTick(Renderer& r) {
    // The decks carry the numbers; the panel shows the verdict.
    bool win = fStats.res && strcmp(fStats.res, "win") == 0;
    const char* v = win ? "GG" : "KO";
    int x0 = (rdW - 11) / 2, y0 = (rdFaceH - 7) / 2;
    r.drawChar(x0,     y0, v[0], win ? 255 : 200);
    r.drawChar(x0 + 6, y0, v[1], win ? 255 : 200);
    rdStage(r, (fHull * 8 + 9) / 10, false);
}

// The device→deck snapshot (~10 Hz via main.cpp / gameNetSnapshot). Idempotent:
// each field is absolute state, so a deck that missed ticks fully re-syncs on
// the next one. Decks filter by role — the glyph is only *shown* on the
// Hacker's deck, the resync key only matters to the Medic reading the
// Hacker's screen, etc.
size_t raidNet(char* buf, size_t cap) {
    if (fState == ST_IDLE) return 0;                     // showcase: no net traffic
    static const char* SN[] = { "idle", "lobby", "intro", "fight", "win", "lose", "stats" };
    bool beamLive = fState == ST_FIGHT && fPhase == F_TELE && fType == T_BEAM && fParty >= 2;
    int n = snprintf(buf, cap,
        "{\"type\":\"raid\",\"st\":\"%s\",\"party\":%u,\"mood\":\"%s\",\"ph\":%u,"
        "\"hp\":%d,\"hull\":%d,\"vulnMs\":%u,\"enrage\":%d,"
        "\"crank\":%u,\"shells\":%u,\"glyph\":%d,\"code\":[%u,%u,%u,%u],"
        "\"acid\":%d,\"acidHp\":%u,\"jam\":%d,\"rsy\":%d,\"lane\":%d,\"laneMs\":%u,"
        "\"ev\":%lu,\"evn\":\"%s\"",
        SN[fState], fParty, MOODS[fMood].name, fPhaseNo,
        (int)fHP, (int)fHull, (unsigned)(fVuln * RD_TICKMS),
        fEnrage < 0 ? -1 : (int)(fEnrage * RD_TICKMS / 1000),
        fCrank, fShells, beamLive ? (int)fGlyph : -1,
        fCode[0], fCode[1], fCode[2], fCode[3],
        (int)fAcid, fAcidHP, (int)fJam, fJam >= 0 ? (int)fResync : -1,
        (int)fLane, (unsigned)(fLane >= 0 ? fLaneT * RD_TICKMS : 0),
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
        case ST_WIN:                                     // the classic row-by-row melt
            fPT++;
            for (int x = 0; x < rdW && x < 16; x++)
                if ((esp_random() % 5) == 0 && rdMelt[x] < rdH) rdMelt[x]++;
            for (int x = 5; x <= 10; x++) { int cy = 12 + rdMelt[x]; if (cy < rdH) r.setPixel(x, cy, 200); }
            for (int e = 0; e < 2; e++) { int bx = e ? 10 : 3;
                for (int y = 3; y <= 5; y++) for (int x = 0; x < 3; x++) {
                    int cy = y + rdMelt[bx + x];
                    if (cy < rdH && !(y == 4 && x == 1)) r.setPixel(bx + x, cy, 200); } }
            if (fPT > 80) { fState = ST_STATS; fPT = 0; fEvent("stats"); }
            break;
        case ST_LOSE:                                    // the grin, then snap to black
            fPT++;
            if (fPT < 30) { rdMouth(r, 0, true); rdBrow(r, 0, true);
                rdEyes(r, 0, false, (fPT & 4) ? 1 : -1); rdNoise(r, 8, rdFaceH); }
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
