#include "games.h"
#include "raid.h"     // Raid 16 boss showcase lives in its own module (raid.cpp)
#include <Arduino.h>
#include <string.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Snake — 1 player. D-pad/swipe steers; walls and your own tail are fatal
// (classic rules). Speeds up as you grow. Death: flash, show the score, any
// key restarts.
// ---------------------------------------------------------------------------

#define GS_MAX 120
static uint8_t  gsX[GS_MAX], gsY[GS_MAX];
static uint8_t  gsLen;
static uint16_t gsHead;
static int8_t   gsDX, gsDY;           // direction actually moving
static int8_t   gsPendDX, gsPendDY;   // latest input, applied at the next step
static uint8_t  gsFoodX, gsFoodY;
static uint8_t  gsPhase;              // 0 play, 1 dying (flash), 2 game over
static uint8_t  gsFlash;
static uint32_t gsLastStep, gsOverAt;
static bool     gsRestart;
static uint16_t gsAnim;

static bool gsOcc(int x, int y) {
    for (int i = 0; i < gsLen; i++) {
        int idx = (gsHead - i + GS_MAX) % GS_MAX;
        if (gsX[idx] == x && gsY[idx] == y) return true;
    }
    return false;
}

static void gsSpawnFood(int W, int H) {
    for (int t = 0; t < 60; t++) {
        int x = esp_random() % W, y = esp_random() % H;
        if (!gsOcc(x, y)) { gsFoodX = x; gsFoodY = y; return; }
    }
    gsFoodX = 0; gsFoodY = 0;
}

static void snakeInit(Renderer& r) {
    int W = r.width(), H = r.height();
    gsLen = 3; gsHead = 2;
    int cx = W / 2 - 1, cy = H / 2;
    for (int i = 0; i < 3; i++) { gsX[i] = cx - 2 + i; gsY[i] = cy; }
    gsDX = 1; gsDY = 0; gsPendDX = 1; gsPendDY = 0;
    gsPhase = 0; gsFlash = 0; gsLastStep = 0; gsRestart = false; gsAnim = 0;
    gsSpawnFood(W, H);
}

static void snakeInput(uint8_t, char k, bool pressed) {
    if (!pressed) return;
    if (gsPhase == 2) {                       // any key restarts (after a beat,
        if (millis() - gsOverAt > 800) gsRestart = true;   // so you see the score)
        return;
    }
    int8_t dx = 0, dy = 0;
    switch (k) {
        case 'U': dy = -1; break;
        case 'D': dy =  1; break;
        case 'L': dx = -1; break;
        case 'R': dx =  1; break;
        default: return;
    }
    if (dx == -gsDX && dy == -gsDY) return;   // no 180s into your own neck
    gsPendDX = dx; gsPendDY = dy;
}

static bool snakeTick(Renderer& r, uint32_t now) {
    int W = r.width(), H = r.height();
    gsAnim++;

    if (gsRestart) { snakeInit(r); r.clear(); return true; }

    if (gsPhase == 1) {                       // death flash
        if (now - gsLastStep < 120) return false;
        gsLastStep = now;
        r.clear();
        uint8_t v = (gsFlash & 1) ? 255 : 40;
        for (int i = 0; i < gsLen; i++) {
            int idx = (gsHead - i + GS_MAX) % GS_MAX;
            r.setPixel(gsX[idx], gsY[idx], v);
        }
        if (++gsFlash >= 7) { gsPhase = 2; gsOverAt = now; gsFlash = 0; }
        return true;
    }

    if (gsPhase == 2) {                       // score screen, blinking gently
        if (now - gsLastStep < 400) return false;
        gsLastStep = now;
        r.clear();
        int score = gsLen - 3; if (score > 99) score = 99;
        char d0 = (char)('0' + score / 10), d1 = (char)('0' + score % 10);
        int x0 = W / 2 - 5, y0 = H / 2 - 4;
        uint8_t v = (gsFlash++ & 1) ? 255 : 170;
        if (score >= 10) r.drawChar(x0, y0, d0, v);
        r.drawChar(score >= 10 ? x0 + 6 : W / 2 - 2, y0, d1, v);
        return true;
    }

    // playing — step interval shrinks as you grow
    uint32_t step = 135 - gsLen * 2; if (step < 70) step = 70;
    if (now - gsLastStep < step) return false;
    gsLastStep = now;

    gsDX = gsPendDX; gsDY = gsPendDY;
    int nx = gsX[gsHead] + gsDX, ny = gsY[gsHead] + gsDY;
    if (nx < 0 || nx >= W || ny < 0 || ny >= H || gsOcc(nx, ny)) {
        gsPhase = 1; gsFlash = 0;
        return false;
    }
    gsHead = (gsHead + 1) % GS_MAX;
    gsX[gsHead] = nx; gsY[gsHead] = ny;
    if (nx == gsFoodX && ny == gsFoodY) {
        if (gsLen < GS_MAX - 4) gsLen++;
        gsSpawnFood(W, H);
    }

    r.clear();
    for (int i = gsLen - 1; i >= 0; i--) {
        int idx = (gsHead - i + GS_MAX) % GS_MAX;
        uint8_t v = i == 0 ? 255 : (uint8_t)(150 - 90 * i / gsLen);
        r.setPixel(gsX[idx], gsY[idx], v);
    }
    r.setPixel(gsFoodX, gsFoodY, (gsAnim & 2) ? 255 : 90);
    return true;
}

// ---------------------------------------------------------------------------
// Pong — up to 2 players. Player 0 = left paddle, player 1 = right. Hold U/D
// to move. A paddle with no human input for a few seconds is driven by the AI
// (deliberately a bit slower than the ball) — so one phone plays vs the house,
// and a second phone can grab the other side mid-game. First to 5 wins.
// ---------------------------------------------------------------------------

static float    pgBX, pgBY, pgVX, pgVY, pgPad[2];
static uint8_t  pgScore[2];
static uint8_t  pgPause, pgWinFlash;
static int8_t   pgWinner;
static uint32_t pgHeld[2][2];      // [player][0=U,1=D] — held until this ms
static uint32_t pgLastHuman[2];    // last human touch per side → AI fallback
static uint32_t pgLastTick;

static void pgServe(int W, int H) {
    pgBX = W * 0.5f; pgBY = H * 0.5f;
    pgVX = (esp_random() & 1) ? 0.55f : -0.55f;
    pgVY = ((int)(esp_random() % 100) - 50) * 0.012f;
}

static void pongInit(Renderer& r) {
    int H = r.height();
    int ph = H / 4; if (ph < 3) ph = 3;
    pgPad[0] = pgPad[1] = (H - ph) * 0.5f;
    pgScore[0] = pgScore[1] = 0;
    pgPause = 10; pgWinFlash = 0; pgWinner = -1;
    memset(pgHeld, 0, sizeof(pgHeld));
    pgLastHuman[0] = pgLastHuman[1] = 0;   // both sides start as AI
    pgLastTick = 0;
    pgServe(r.width(), H);
}

static void pongInput(uint8_t p, char k, bool pressed) {
    if (p > 1) p = 1;
    int idx;
    if      (k == 'U') idx = 0;
    else if (k == 'D') idx = 1;
    else return;
    // Held keys expire on their own (1.2 s) in case a release event is lost on
    // WiFi; the play page re-sends presses while held, refreshing this.
    pgHeld[p][idx] = pressed ? millis() + 1200 : 0;
    pgLastHuman[p] = millis();
}

static bool pongTick(Renderer& r, uint32_t now) {
    if (now - pgLastTick < 45) return false;
    pgLastTick = now;
    int W = r.width(), H = r.height();
    int ph = H / 4; if (ph < 3) ph = 3;

    if (pgWinFlash) {                          // match over: winner side flashes
        pgWinFlash--;
        r.clear();
        if (pgWinFlash & 2) {
            int x0 = pgWinner == 0 ? 0 : W / 2;
            for (int y = 0; y < H; y++)
                for (int x = x0; x < x0 + W / 2; x++) r.setPixel(x, y, 120);
        }
        if (!pgWinFlash) pongInit(r);
        return true;
    }

    // paddles: human while keys are held / recently touched, AI otherwise
    for (int p = 0; p < 2; p++) {
        bool u = pgHeld[p][0] > now, d = pgHeld[p][1] > now;
        if (u && !d) pgPad[p] -= 0.62f;
        else if (d && !u) pgPad[p] += 0.62f;
        else if (now - pgLastHuman[p] > 4000) {
            bool chasing = (p == 0) ? pgVX < 0 : pgVX > 0;
            float target = chasing ? pgBY - ph * 0.5f : (H - ph) * 0.5f;
            float diff = target - pgPad[p];
            float cap = chasing ? 0.42f : 0.10f;
            pgPad[p] += diff > cap ? cap : (diff < -cap ? -cap : diff);
        }
        if (pgPad[p] < 0) pgPad[p] = 0;
        if (pgPad[p] > H - ph) pgPad[p] = H - ph;
    }

    if (pgPause) {
        if (--pgPause == 0) pgServe(W, H);
    } else {
        pgBX += pgVX; pgBY += pgVY;
        if (pgBY < 0)     { pgBY = 0;     pgVY = -pgVY; }
        if (pgBY > H - 1) { pgBY = H - 1; pgVY = -pgVY; }
        if (pgBX <= 1 && pgVX < 0) {
            if (pgBY >= pgPad[0] - 0.5f && pgBY <= pgPad[0] + ph + 0.5f) {
                pgVX = -pgVX * 1.04f; if (pgVX > 0.95f) pgVX = 0.95f;
                pgVY += ((int)(esp_random() % 100) - 50) * 0.008f;
                pgBX = 1;
            } else if (pgBX < -2) {
                pgScore[1]++; pgPause = 16;
                if (pgScore[1] >= 5) { pgWinner = 1; pgWinFlash = 24; }
            }
        }
        if (pgBX >= W - 2 && pgVX > 0) {
            if (pgBY >= pgPad[1] - 0.5f && pgBY <= pgPad[1] + ph + 0.5f) {
                pgVX = -pgVX * 1.04f; if (pgVX < -0.95f) pgVX = -0.95f;
                pgVY += ((int)(esp_random() % 100) - 50) * 0.008f;
                pgBX = W - 2;
            } else if (pgBX > W + 1) {
                pgScore[0]++; pgPause = 16;
                if (pgScore[0] >= 5) { pgWinner = 0; pgWinFlash = 24; }
            }
        }
    }

    r.clear();
    for (int y = 0; y < H; y += 3) r.setPixel(W / 2, y, 36);          // net
    for (int i = 0; i < pgScore[0] && i < 5; i++) r.setPixel(1 + i, 0, 130);      // score pips
    for (int i = 0; i < pgScore[1] && i < 5; i++) r.setPixel(W - 2 - i, 0, 130);
    for (int y = 0; y < ph; y++) {
        r.setPixel(0,     (int)pgPad[0] + y, 255);
        r.setPixel(W - 1, (int)pgPad[1] + y, 255);
    }
    if (!pgPause) r.setPixel((int)(pgBX + 0.5f), (int)(pgBY + 0.5f), 255);
    return true;
}

// ---------------------------------------------------------------------------
// Tetris — 1 player. The standard 7 tetrominoes (I, O, T, S, Z, J, L — the
// complete set of 4-cell shapes, identical in every version of Tetris) with
// the original's simple rotation: rotate in place, reject if it collides, no
// wall kicks. A 10-wide well (centered on the canvas) as tall as the canvas
// allows. Falling piece is full-bright, the settled stack is dimmer, and a
// faint ghost shows where the piece will land. L/R shift, U rotates, D soft-
// drops (hold), A hard-drops. Full rows flash then clear; toppng out flashes
// the stack and restarts. Falls faster as your line count climbs.
// ---------------------------------------------------------------------------

#define TET_W_MAX 10          // classic well width
#define TET_H_MAX 64          // caps the static board (≥ 4 stacked panels tall)
static uint8_t  teBoard[TET_H_MAX][TET_W_MAX];   // 0 = empty, else cell brightness
static int      teW, teH, teX0;                  // well geometry on the canvas
static int8_t   tePiece, teRot, tePX, tePY, teNext;
static uint16_t teLines;
static uint32_t teLastStep, teSoftUntil, tePhaseTimer;
static uint8_t  tePhase;      // 0 playing, 1 line-clear flash, 2 game over
static uint8_t  teFlash;
static int8_t   teClearRows[4];
static uint8_t  teClearN;
static bool     teDirty;

// [7 pieces][4 rotations][4 cells][col,row] in a 4x4 box. Standard spawn
// orientations; rotation index advances clockwise. O is identical in all four.
static const int8_t TET_PIECES[7][4][4][2] = {
    // I
    {{{0,1},{1,1},{2,1},{3,1}}, {{2,0},{2,1},{2,2},{2,3}}, {{0,2},{1,2},{2,2},{3,2}}, {{1,0},{1,1},{1,2},{1,3}}},
    // O
    {{{1,0},{2,0},{1,1},{2,1}}, {{1,0},{2,0},{1,1},{2,1}}, {{1,0},{2,0},{1,1},{2,1}}, {{1,0},{2,0},{1,1},{2,1}}},
    // T
    {{{1,0},{0,1},{1,1},{2,1}}, {{1,0},{1,1},{2,1},{1,2}}, {{0,1},{1,1},{2,1},{1,2}}, {{1,0},{0,1},{1,1},{1,2}}},
    // S
    {{{1,0},{2,0},{0,1},{1,1}}, {{1,0},{1,1},{2,1},{2,2}}, {{1,1},{2,1},{0,2},{1,2}}, {{0,0},{0,1},{1,1},{1,2}}},
    // Z
    {{{0,0},{1,0},{1,1},{2,1}}, {{2,0},{1,1},{2,1},{1,2}}, {{0,1},{1,1},{1,2},{2,2}}, {{1,0},{0,1},{1,1},{0,2}}},
    // J
    {{{0,0},{0,1},{1,1},{2,1}}, {{1,0},{2,0},{1,1},{1,2}}, {{0,1},{1,1},{2,1},{2,2}}, {{1,0},{1,1},{0,2},{1,2}}},
    // L
    {{{2,0},{0,1},{1,1},{2,1}}, {{1,0},{1,1},{1,2},{2,2}}, {{0,1},{1,1},{2,1},{0,2}}, {{0,0},{1,0},{1,1},{1,2}}},
};

static bool teCollide(int type, int rot, int px, int py) {
    for (int i = 0; i < 4; i++) {
        int cx = px + TET_PIECES[type][rot][i][0];
        int cy = py + TET_PIECES[type][rot][i][1];
        if (cx < 0 || cx >= teW || cy >= teH) return true;
        if (cy >= 0 && teBoard[cy][cx]) return true;   // cy<0 (above the well) is free — lets pieces spawn
    }
    return false;
}

static void teSpawn() {
    tePiece = teNext;
    teNext  = esp_random() % 7;
    teRot   = 0;
    tePX    = teW / 2 - 2;   // 4-wide box centered
    tePY    = 0;
    teSoftUntil = 0;
    if (teCollide(tePiece, teRot, tePX, tePY)) { tePhase = 2; teFlash = 0; tePhaseTimer = millis(); }
    teDirty = true;
}

static void tetrisReset(Renderer& r) {
    int W = r.width(), H = r.height();
    teW  = W < TET_W_MAX ? W : TET_W_MAX;
    teH  = H < TET_H_MAX ? H : TET_H_MAX;
    teX0 = (W - teW) / 2;
    memset(teBoard, 0, sizeof(teBoard));
    teLines = 0; tePhase = 0; teLastStep = 0; teSoftUntil = 0; teFlash = 0; teClearN = 0;
    teNext = esp_random() % 7;
    teSpawn();
}

static void teLock() {
    for (int i = 0; i < 4; i++) {
        int cx = tePX + TET_PIECES[tePiece][teRot][i][0];
        int cy = tePY + TET_PIECES[tePiece][teRot][i][1];
        if (cx >= 0 && cx < teW && cy >= 0 && cy < teH) teBoard[cy][cx] = 190;
    }
    teClearN = 0;
    for (int y = 0; y < teH && teClearN < 4; y++) {
        bool full = true;
        for (int x = 0; x < teW; x++) if (!teBoard[y][x]) { full = false; break; }
        if (full) teClearRows[teClearN++] = y;
    }
    if (teClearN > 0) { tePhase = 1; teFlash = 0; tePhaseTimer = millis(); }
    else teSpawn();
}

// Remove the flagged full rows and drop everything above them down. Rows are
// flagged top-to-bottom; removing an upper row never shifts anything below it,
// so the remaining (lower) flags stay valid as we go.
static void teDoClear() {
    for (uint8_t c = 0; c < teClearN; c++) {
        int y = teClearRows[c];
        for (int yy = y; yy > 0; yy--)
            for (int x = 0; x < teW; x++) teBoard[yy][x] = teBoard[yy - 1][x];
        for (int x = 0; x < teW; x++) teBoard[0][x] = 0;
    }
    teLines += teClearN;
    teClearN = 0;
    teSpawn();
}

static void teGravity() {
    if (!teCollide(tePiece, teRot, tePX, tePY + 1)) tePY++;
    else teLock();
    teDirty = true;
}

static int teLevelDrop() {           // fall interval, shrinks every 10 lines
    int lvl = teLines / 10;
    int d = 550 - lvl * 45;
    return d < 90 ? 90 : d;
}

static void tetrisInit(Renderer& r) { tetrisReset(r); }

static void tetrisInput(uint8_t, char k, bool pressed) {
    if (tePhase == 2) return;   // game-over animation auto-restarts; ignore input
    if (k == 'D') {             // soft drop is a held action (like Pong paddles)
        teSoftUntil = pressed ? millis() + 1200 : 0;   // 1.2s expiry covers a lost release
        return;
    }
    if (!pressed || tePhase != 0) return;
    switch (k) {
        case 'L': if (!teCollide(tePiece, teRot, tePX - 1, tePY)) tePX--; break;
        case 'R': if (!teCollide(tePiece, teRot, tePX + 1, tePY)) tePX++; break;
        case 'U': { int nr = (teRot + 1) & 3;                         // rotate CW, no wall kicks
                    if (!teCollide(tePiece, nr, tePX, tePY)) teRot = nr; break; }
        case 'A': while (!teCollide(tePiece, teRot, tePX, tePY + 1)) tePY++;   // hard drop
                  teLock(); break;
    }
    teDirty = true;
}

static void teDrawWalls(Renderer& r) {
    int W = r.width();
    for (int y = 0; y < teH; y++) {
        if (teX0 - 1 >= 0)      r.setPixel(teX0 - 1,   y, 25);
        if (teX0 + teW < W)     r.setPixel(teX0 + teW, y, 25);
    }
}

static void teRender(Renderer& r) {
    r.clear();
    teDrawWalls(r);
    for (int y = 0; y < teH; y++)
        for (int x = 0; x < teW; x++)
            if (teBoard[y][x]) r.setPixel(teX0 + x, y, teBoard[y][x]);
    // ghost: where the piece will land (only on empty cells)
    int gy = tePY; while (!teCollide(tePiece, teRot, tePX, gy + 1)) gy++;
    for (int i = 0; i < 4; i++) {
        int cx = teX0 + tePX + TET_PIECES[tePiece][teRot][i][0];
        int cy = gy + TET_PIECES[tePiece][teRot][i][1];
        if (cy >= 0 && r.getPixel(cx, cy) == 0) r.setPixel(cx, cy, 40);
    }
    // active falling piece
    for (int i = 0; i < 4; i++) {
        int cx = teX0 + tePX + TET_PIECES[tePiece][teRot][i][0];
        int cy = tePY + TET_PIECES[tePiece][teRot][i][1];
        if (cy >= 0) r.setPixel(cx, cy, 255);
    }
}

static void teRenderClearing(Renderer& r, bool on) {
    r.clear();
    teDrawWalls(r);
    for (int y = 0; y < teH; y++) {
        bool clearing = false;
        for (uint8_t c = 0; c < teClearN; c++) if (teClearRows[c] == y) clearing = true;
        for (int x = 0; x < teW; x++) {
            if (clearing)             r.setPixel(teX0 + x, y, on ? 255 : 0);
            else if (teBoard[y][x])   r.setPixel(teX0 + x, y, teBoard[y][x]);
        }
    }
}

static bool tetrisTick(Renderer& r, uint32_t now) {
    if (tePhase == 2) {                 // game over: flash the stack, then restart
        if (now - tePhaseTimer < 130) return false;
        tePhaseTimer = now;
        if (++teFlash >= 8) { tetrisReset(r); return true; }
        r.clear();
        teDrawWalls(r);
        uint8_t v = (teFlash & 1) ? 200 : 40;
        for (int y = 0; y < teH; y++)
            for (int x = 0; x < teW; x++)
                if (teBoard[y][x]) r.setPixel(teX0 + x, y, v);
        return true;
    }

    if (tePhase == 1) {                 // line-clear flash, then compact
        if (now - tePhaseTimer < 100) return false;
        tePhaseTimer = now;
        if (++teFlash >= 6) { teDoClear(); teRender(r); return true; }
        teRenderClearing(r, teFlash & 1);
        return true;
    }

    // playing — gravity, then render if anything moved
    uint32_t drop = (now < teSoftUntil) ? 45 : (uint32_t)teLevelDrop();
    if (now - teLastStep >= drop) { teLastStep = now; teGravity(); }
    if (teDirty) { teDirty = false; teRender(r); return true; }
    return false;
}

// ---------------------------------------------------------------------------
// Pip Run — 1 player. The Chrome "dino" runner, but it's Pip: he dashes right
// across the ground under a starry sky while cacti scroll toward him; one
// button hops him over them. Any press jumps; miss a cactus and he tumbles,
// flashes, and it restarts (or press to restart immediately). It speeds up the
// longer you survive. Pip's sprite (glowing body, an OFF pixel for the eye,
// antenna, stubby feet) matches the idle critter elsewhere on the device.
// ---------------------------------------------------------------------------

#define PIPR_W 5
#define PIPR_H 5
// Cell codes match Pip elsewhere: 0 none, 1 body, 2 eye (drawn OFF), 3 antenna
// tip, 4 foot. Facing right (the run direction). Two run frames waddle the
// legs + antenna; the jump pose kicks both legs out.
static const uint8_t PIPR_RUN[2][PIPR_H][PIPR_W] = {
    { {0,0,3,0,0},{0,1,1,1,0},{0,1,1,2,0},{0,1,1,1,0},{0,4,0,4,0} },
    { {0,0,0,3,0},{0,1,1,1,0},{0,1,1,2,0},{0,1,1,1,0},{0,0,4,4,0} },
};
static const uint8_t PIPR_JUMP[PIPR_H][PIPR_W] = {
    {0,0,3,0,0},{0,1,1,1,0},{0,1,1,2,0},{0,1,1,1,0},{4,0,0,0,4},
};

#define PR_MAXOB   4          // cacti tracked at once
#define PR_STARS   6
#define PR_PIPX    2          // Pip's fixed left-edge column
#define PR_GRAV    0.30f
#define PR_JUMP    1.75f
#define PR_TICKMS  55

static int      prW, prH, prGY;              // canvas dims + ground row
static float    prPipY, prVY;                // Pip top row (float) + vertical velocity
static bool     prGrounded;
static float    prObX[PR_MAXOB];             // obstacle left x (float)
static uint8_t  prObW[PR_MAXOB], prObH[PR_MAXOB];
static bool     prObUsed[PR_MAXOB];
static float    prSpeed;                     // px per tick (ramps with score)
static float    prNextGap;                   // px gap before the next spawn
static uint32_t prScore;
static float    prScroll;                    // ground-texture scroll
static uint8_t  prStarX[PR_STARS], prStarY[PR_STARS], prStarPh[PR_STARS];
static uint32_t prLastTick, prPhaseTimer;
static uint8_t  prPhase;                     // 0 running, 1 crashed
static uint8_t  prFlash;
static uint16_t prTick;

static inline float prPipRestY() { return (float)(prGY - (PIPR_H - 1)); }

static void prSpawn() {
    for (int i = 0; i < PR_MAXOB; i++) {
        if (prObUsed[i]) continue;
        prObUsed[i] = true;
        prObX[i]    = (float)prW + 1.0f;             // enter from just off the right edge
        prObW[i]    = 1 + (esp_random() & 1);        // 1–2 wide
        prObH[i]    = 2 + (esp_random() % 2);        // 2–3 tall (jump peak clears 3)
        return;
    }
}

// Resets game state using the dimensions captured at init — so the input
// handler (which gets no Renderer) can restart after a crash just like the tick.
static void prResetState() {
    prPipY = prPipRestY(); prVY = 0; prGrounded = true;
    for (int i = 0; i < PR_MAXOB; i++) prObUsed[i] = false;
    prSpeed = 0.75f; prNextGap = 10.0f; prScore = 0; prScroll = 0;
    prPhase = 0; prFlash = 0; prTick = 0; prLastTick = 0;
    // A starry sky fills the space above the track (charming on the tall
    // 2-panel canvas; a couple of stars even at 16px). Kept clear of the
    // ground band so nothing reads as a floating obstacle.
    int skyH = prGY - PIPR_H - 1; if (skyH < 1) skyH = 1;
    for (int i = 0; i < PR_STARS; i++) {
        prStarX[i]  = esp_random() % (prW > 0 ? prW : 1);
        prStarY[i]  = esp_random() % skyH;
        prStarPh[i] = esp_random() % 6;
    }
}

static void dinoInit(Renderer& r) {
    prW = r.width(); prH = r.height(); prGY = prH - 1;
    prResetState();
}

static void dinoInput(uint8_t, char, bool pressed) {
    if (!pressed) return;
    if (prPhase == 1) { prResetState(); return; }   // press to restart after a crash
    if (prGrounded)   { prVY = -PR_JUMP; prGrounded = false; }
}

static void prDrawPip(Renderer& r, const uint8_t (*spr)[PIPR_W]) {
    int top = (int)(prPipY + 0.5f);
    for (int row = 0; row < PIPR_H; row++)
        for (int col = 0; col < PIPR_W; col++) {
            uint8_t c = spr[row][col];
            if (c == 0 || c == 2) continue;                 // none, or the eye (OFF hole)
            uint8_t v = c == 1 ? 200 : (c == 3 ? 255 : 150); // body / antenna tip / foot
            r.setPixel(PR_PIPX + col, top + row, v);
        }
}

static bool dinoTick(Renderer& r, uint32_t now) {
    if (now - prLastTick < PR_TICKMS) return false;
    prLastTick = now;

    if (prPhase == 1) {                        // crash: flash Pip, then auto-restart
        prFlash++;
        r.clear();
        for (int x = 0; x < prW; x++) r.setPixel(x, prGY, 30);
        if (prFlash & 1) prDrawPip(r, PIPR_JUMP);
        if (prFlash >= 10) prResetState();
        return true;
    }

    prTick++;

    // --- physics: gravity + jump arc ---
    if (!prGrounded) {
        prVY += PR_GRAV;
        prPipY += prVY;
        if (prPipY >= prPipRestY()) { prPipY = prPipRestY(); prVY = 0; prGrounded = true; }
    }

    // --- scroll obstacles, recycle, spawn with a gap ---
    prScroll += prSpeed;
    float rightmost = -1000.0f;
    for (int i = 0; i < PR_MAXOB; i++) {
        if (!prObUsed[i]) continue;
        prObX[i] -= prSpeed;
        if (prObX[i] + prObW[i] < 0) { prObUsed[i] = false; continue; }
        if (prObX[i] > rightmost) rightmost = prObX[i];
    }
    if (rightmost < (float)prW - prNextGap) {
        prSpawn();
        prNextGap = 9.0f + (float)(esp_random() % 9);       // 9–17 px until the next
    }

    // --- score + difficulty ramp ---
    prScore++;
    prSpeed = 0.75f + (float)prScore * 0.0016f;             // eases up over ~minutes
    if (prSpeed > 1.9f) prSpeed = 1.9f;

    // --- collision (AABB, Pip's body box vs each cactus) ---
    int pipL = PR_PIPX + 1, pipR = PR_PIPX + 3;
    int pipT = (int)(prPipY + 0.5f) + 1, pipB = (int)(prPipY + 0.5f) + 4;
    for (int i = 0; i < PR_MAXOB; i++) {
        if (!prObUsed[i]) continue;
        int cL = (int)(prObX[i] + 0.5f), cR = cL + prObW[i] - 1;
        int cT = prGY - prObH[i] + 1, cB = prGY;
        if (pipR >= cL && pipL <= cR && pipB >= cT && pipT <= cB) {
            prPhase = 1; prFlash = 0;                        // crash
            return true;
        }
    }

    // --- render ---
    r.clear();
    // sky: twinkling stars (blink OFF/ON — a temporal cue the LEDs render well)
    for (int i = 0; i < PR_STARS; i++) {
        if (((prTick / 6 + prStarPh[i]) % 5) != 0)
            r.setPixel(prStarX[i], prStarY[i], 45);
    }
    // ground: dim line with a scrolling dash for a sense of speed
    for (int x = 0; x < prW; x++) {
        bool dash = (((x + (int)prScroll) & 3) == 0);
        r.setPixel(x, prGY, dash ? 70 : 25);
    }
    // cacti
    for (int i = 0; i < PR_MAXOB; i++) {
        if (!prObUsed[i]) continue;
        int cL = (int)(prObX[i] + 0.5f);
        for (int h = 0; h < prObH[i]; h++)
            for (int w = 0; w < prObW[i]; w++)
                r.setPixel(cL + w, prGY - h, 200);
        if (prObH[i] >= 2 && cL - 1 >= 0) r.setPixel(cL - 1, prGY - 1, 200);  // a little arm
    }
    // Pip
    prDrawPip(r, prGrounded ? PIPR_RUN[(prTick >> 1) & 1] : PIPR_JUMP);
    return true;
}

// ---------------------------------------------------------------------------
// Flappy Pip — 1 player. Flappy Bird with Pip: gravity pulls him down, each
// press flaps him up, and he must thread the gaps in walls scrolling toward
// him. Touch a wall or the floor and he tumbles (flash → auto-restart, or
// press to restart). Suits the tall 2-panel canvas especially well. Reuses
// Pip's palette; a wings-up frame while rising and a feet-down frame while
// falling give the flap its feel.
// ---------------------------------------------------------------------------

// 5x5, cell codes as elsewhere (0 none, 1 body, 2 eye OFF, 3 antenna, 4 wing/foot).
static const uint8_t PFLAP_UP[PIPR_H][PIPR_W] = {   // just flapped — wings up
    {0,0,3,0,0},{4,1,1,1,0},{0,1,1,2,4},{0,1,1,1,0},{0,0,0,0,0},
};
static const uint8_t PFLAP_DOWN[PIPR_H][PIPR_W] = { // falling — feet dangle
    {0,0,3,0,0},{0,1,1,1,0},{0,1,1,2,0},{0,1,1,1,0},{0,4,0,4,0},
};

#define PF_MAXP    4          // walls tracked at once
#define PF_PIPX    3          // Pip's fixed left-edge column
// Gentle-ish for a tiny fast screen: a flap rises ~4px, terminal fall is slow
// enough that ~1.5 taps/sec hovers. Flappy Bird is punishing by nature — tune
// here if it feels too hard (lower GRAV / VYMAX) or too easy (raise them).
#define PF_GRAV    0.11f
#define PF_FLAP    0.98f      // upward velocity set on each flap
#define PF_VYMAX   1.30f      // terminal fall speed
#define PF_TICKMS  55

static int      pfW, pfH;
static float    pfY, pfVY;                   // Pip top row (float) + velocity
static float    pfPipeX[PF_MAXP];            // wall left x
static uint8_t  pfGapY[PF_MAXP], pfGapH[PF_MAXP], pfPipeW[PF_MAXP];
static bool     pfUsed[PF_MAXP], pfScored[PF_MAXP];
static float    pfSpeed, pfNextGap;
static uint32_t pfScore;
static uint8_t  pfStarX[PR_STARS], pfStarY[PR_STARS], pfStarPh[PR_STARS];
static uint32_t pfLastTick;
static uint8_t  pfPhase, pfFlash;
static uint16_t pfTick;

static int pfGapHForScore() {                // gap tightens as you score, to a floor
    int base = pfH / 3; if (base < 6) base = 6;
    int g = base - (int)(pfScore / 4);
    int lo = pfH <= 16 ? 6 : 8;
    return g < lo ? lo : g;
}

static void pfSpawn() {
    for (int i = 0; i < PF_MAXP; i++) {
        if (pfUsed[i]) continue;
        pfUsed[i] = true; pfScored[i] = false;
        pfPipeX[i] = (float)pfW + 1.0f;
        pfPipeW[i] = 1 + (esp_random() & 1);           // 1–2 wide
        int gh = pfGapHForScore();
        pfGapH[i]  = gh;
        int span = pfH - gh - 2; if (span < 1) span = 1;
        pfGapY[i]  = 1 + esp_random() % span;          // gap top (leaves a rim top & bottom)
        return;
    }
}

static void pfResetState() {
    pfY = (float)(pfH / 2 - 2); pfVY = 0;
    for (int i = 0; i < PF_MAXP; i++) pfUsed[i] = false;
    pfSpeed = 0.7f; pfNextGap = 11.0f; pfScore = 0;
    pfPhase = 0; pfFlash = 0; pfTick = 0; pfLastTick = 0;
    for (int i = 0; i < PR_STARS; i++) {
        pfStarX[i]  = esp_random() % (pfW > 0 ? pfW : 1);
        pfStarY[i]  = esp_random() % (pfH > 0 ? pfH : 1);
        pfStarPh[i] = esp_random() % 6;
    }
}

static void flappyInit(Renderer& r) {
    pfW = r.width(); pfH = r.height();
    pfResetState();
}

static void flappyInput(uint8_t, char, bool pressed) {
    if (!pressed) return;
    if (pfPhase == 1) { pfResetState(); return; }      // press to restart after a crash
    pfVY = -PF_FLAP;                                    // flap!
}

static void pfDrawPip(Renderer& r, const uint8_t (*spr)[PIPR_W]) {
    int top = (int)(pfY + 0.5f);
    for (int row = 0; row < PIPR_H; row++)
        for (int col = 0; col < PIPR_W; col++) {
            uint8_t c = spr[row][col];
            if (c == 0 || c == 2) continue;
            uint8_t v = c == 1 ? 200 : (c == 3 ? 255 : 150);
            r.setPixel(PF_PIPX + col, top + row, v);
        }
}

static bool flappyTick(Renderer& r, uint32_t now) {
    if (now - pfLastTick < PF_TICKMS) return false;
    pfLastTick = now;

    if (pfPhase == 1) {                                 // crash: flash Pip, then restart
        pfFlash++;
        r.clear();
        for (int x = 0; x < pfW; x++) r.setPixel(x, pfH - 1, 30);
        if (pfFlash & 1) pfDrawPip(r, PFLAP_DOWN);
        if (pfFlash >= 10) pfResetState();
        return true;
    }

    pfTick++;

    // --- physics ---
    pfVY += PF_GRAV;
    if (pfVY > PF_VYMAX) pfVY = PF_VYMAX;
    pfY += pfVY;
    if (pfY < 0) { pfY = 0; pfVY = 0; }                 // bonk the ceiling (no death)

    // --- scroll walls, recycle, spawn, score ---
    pfSpeed = 0.7f + (float)pfScore * 0.02f;
    if (pfSpeed > 1.7f) pfSpeed = 1.7f;
    float rightmost = -1000.0f;
    for (int i = 0; i < PF_MAXP; i++) {
        if (!pfUsed[i]) continue;
        pfPipeX[i] -= pfSpeed;
        if (pfPipeX[i] + pfPipeW[i] < 0) { pfUsed[i] = false; continue; }
        if (!pfScored[i] && pfPipeX[i] + pfPipeW[i] < PF_PIPX) { pfScored[i] = true; pfScore++; }
        if (pfPipeX[i] > rightmost) rightmost = pfPipeX[i];
    }
    if (rightmost < (float)pfW - pfNextGap) {
        pfSpawn();
        pfNextGap = 9.0f + (float)(esp_random() % 7);
    }

    // --- collision: Pip's body box vs walls + floor ---
    int pipT = (int)(pfY + 0.5f) + 1, pipB = (int)(pfY + 0.5f) + 4;
    int pipL = PF_PIPX + 1, pipR = PF_PIPX + 3;
    bool dead = (pipB >= pfH - 1);                      // floor
    for (int i = 0; i < PF_MAXP && !dead; i++) {
        if (!pfUsed[i]) continue;
        int wL = (int)(pfPipeX[i] + 0.5f), wR = wL + pfPipeW[i] - 1;
        if (pipR < wL || pipL > wR) continue;           // not overlapping this wall's columns
        // safe only if Pip is fully inside the gap
        if (pipT < pfGapY[i] || pipB > pfGapY[i] + pfGapH[i] - 1) dead = true;
    }
    if (dead) { pfPhase = 1; pfFlash = 0; return true; }

    // --- render ---
    r.clear();
    for (int i = 0; i < PR_STARS; i++)
        if (((pfTick / 6 + pfStarPh[i]) % 5) != 0)
            r.setPixel(pfStarX[i], pfStarY[i], 45);
    for (int x = 0; x < pfW; x++) r.setPixel(x, pfH - 1, 30);   // ground line
    for (int i = 0; i < PF_MAXP; i++) {
        if (!pfUsed[i]) continue;
        int wL = (int)(pfPipeX[i] + 0.5f);
        for (int w = 0; w < pfPipeW[i]; w++) {
            int x = wL + w; if (x < 0 || x >= pfW) continue;
            for (int y = 0; y < pfGapY[i]; y++)                 r.setPixel(x, y, 200);  // top wall
            for (int y = pfGapY[i] + pfGapH[i]; y < pfH; y++)   r.setPixel(x, y, 200);  // bottom wall
        }
        // bright gap rims so the opening reads clearly on the LEDs
        int rimx = wL + pfPipeW[i] - 1; if (rimx >= 0 && rimx < pfW) {
            if (pfGapY[i] - 1 >= 0)                r.setPixel(rimx, pfGapY[i] - 1, 255);
            if (pfGapY[i] + pfGapH[i] < pfH)       r.setPixel(rimx, pfGapY[i] + pfGapH[i], 255);
        }
    }
    pfDrawPip(r, pfVY < 0 ? PFLAP_UP : PFLAP_DOWN);
    return true;
}

// ---------------------------------------------------------------------------
// Registry — one row per game. /api/games serves this list; the play page's
// picker builds itself from it.
// ---------------------------------------------------------------------------

struct GameDef {
    GameInfo info;
    void (*init)(Renderer&);
    void (*input)(uint8_t, char, bool);
    bool (*tick)(Renderer&, uint32_t);
    // Optional device→deck snapshot publisher (see gameNetSnapshot in games.h).
    // nullptr = this game has no net layer.
    size_t (*net)(char*, size_t);
};

static const GameDef GAMES[] = {
    { { "snake",  "Snake",      1 }, snakeInit,  snakeInput,  snakeTick,  nullptr },
    { { "pong",   "Pong",       2 }, pongInit,   pongInput,   pongTick,   nullptr },
    { { "tetris", "Tetris",     1 }, tetrisInit, tetrisInput, tetrisTick, nullptr },
    { { "dino",   "Pip Run",    1 }, dinoInit,   dinoInput,   dinoTick,   nullptr },
    { { "flappy", "Flappy Pip", 1 }, flappyInit, flappyInput, flappyTick, nullptr },
    { { "raid",   "Raid 16",    4 }, raidInit,   raidInput,   raidTick,   raidNet },
};
static const uint8_t NGAMES = sizeof(GAMES) / sizeof(GAMES[0]);
static int8_t curGame = -1;

uint8_t         gameCount()          { return NGAMES; }
const GameInfo& gameInfo(uint8_t i)  { return GAMES[i < NGAMES ? i : 0].info; }

bool gameStart(const char* name, Renderer& r) {
    for (uint8_t i = 0; i < NGAMES; i++)
        if (strcmp(GAMES[i].info.name, name) == 0) {
            curGame = i;
            GAMES[i].init(r);
            return true;
        }
    curGame = -1;
    return false;
}

void gameInput(uint8_t player, char key, bool pressed) {
    if (curGame >= 0) GAMES[curGame].input(player, key, pressed);
}

bool gameTick(Renderer& r, uint32_t nowMs) {
    return curGame >= 0 && GAMES[curGame].tick(r, nowMs);
}

size_t gameNetSnapshot(char* buf, size_t cap) {
    return (curGame >= 0 && GAMES[curGame].net) ? GAMES[curGame].net(buf, cap) : 0;
}
