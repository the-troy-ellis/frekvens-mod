#include "effects.h"
#include <Arduino.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Shared helpers / state
// ---------------------------------------------------------------------------

static uint32_t effTick = 0;   // increments once per effectTick

// Multiply every lit pixel by num/den — the standard trail/afterglow fade.
static void fadeCanvas(Renderer& r, uint8_t num, uint8_t den) {
    int W = r.width(), H = r.height();
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint8_t v = r.getPixel(x, y);
            if (v) r.setPixel(x, y, (uint8_t)((uint16_t)v * num / den));
        }
}

static inline float frand() { return (float)(esp_random() % 1000) / 1000.0f; }

// --- classics ---
static uint16_t sweepCol = 0;
static int16_t  rainDrop[CANVAS_MAX_W];
static uint8_t* fireHeat    = nullptr;   // W * (H+1), +1 fuel row below the view
static uint8_t* lifeGrid[2] = { nullptr, nullptr };
static uint8_t  lifeCur = 0;
static uint16_t lifeGen = 0;
static uint32_t plasmaT = 0;

// --- bars (VU meter) ---
#define MAX_BARS 42
static float barLvl[MAX_BARS], barTgt[MAX_BARS], barPeak[MAX_BARS];

// --- scope ---
static float scopePhase = 0, scopeT = 0;

// --- bounce ---
static float ballX, ballY, ballVX, ballVY;

// --- snake ---
#define SNAKE_MAX 96
static uint8_t  snakeX[SNAKE_MAX], snakeY[SNAKE_MAX];
static uint8_t  snakeLen;
static uint16_t snakeHead;      // ring-buffer index of the head
static int8_t   snakeDX, snakeDY;
static uint8_t  foodX, foodY;
static uint8_t  snakeDead;      // death-flash countdown

// --- pong ---
static float   pongBX, pongBY, pongVX, pongVY, padL, padR;
static uint8_t pongPause;

// --- blobs ---
static float blobX[3], blobY[3], blobVX[3], blobVY[3];
static const float blobR2[3] = { 10.0f, 16.0f, 7.0f };

// --- ripple ---
struct Ring { float r; int16_t str; uint8_t cx, cy, on; };
static Ring rings[6];

// --- warp ---
struct Star { float x, y, vx, vy; };
static Star stars[24];

// --- fireworks ---
struct Spark { float x, y, vx, vy; int16_t life; };
static Spark   sparks[28];
static float   rocketX, rocketY, rocketVY;
static uint8_t fwPhase, fwPause;   // 0 = rocket climbing, 1 = burst

// --- sand ---
static float   sandSpout;
static uint8_t sandFull;

// --- radar ---
static float radarA = 0;
struct Blip { uint8_t x, y, on; };
static Blip blips[4];

// --- heart / disco / static ---
static uint16_t heartTick = 0;
static uint16_t discoTick = 0;
static uint8_t  discoPat  = 0;
static uint16_t tvRoll    = 0;

// 16x16 heart, one uint16_t per row, bit 15 = leftmost.
static const uint16_t HEART_ICON[16] = {
    0x0000, 0x0000, 0x0000, 0x1C70, 0x3EF8, 0x7FFC, 0x7FFC, 0x7FFC,
    0x3FF8, 0x1FF0, 0x0FE0, 0x07C0, 0x0380, 0x0100, 0x0000, 0x0000,
};

// --- Pip, the critter -------------------------------------------------------
// A 6x5 blob with a wiggling antenna, a single dark eye punched into his
// glowing body (the eye is an OFF pixel — reads beautifully on LEDs), and two
// stubby feet. Sprites face right; walking left mirrors them so the eye stays
// on the leading edge. Cell codes: 0 none, 1 body, 2 eye, 3 antenna tip, 4 foot.
#define PIP_W 6
#define PIP_H 5
static const uint8_t PIP_WALK[2][PIP_H][PIP_W] = {
    { {0,0,3,0,0,0},          // step A: feet apart, tip left
      {0,1,1,1,1,0},
      {0,1,1,1,2,0},
      {0,1,1,1,1,0},
      {0,4,0,0,4,0} },
    { {0,0,0,3,0,0},          // step B: feet tucked, tip right (waddle!)
      {0,1,1,1,1,0},
      {0,1,1,1,2,0},
      {0,1,1,1,1,0},
      {0,0,4,4,0,0} },
};
static const uint8_t PIP_FACE[PIP_H][PIP_W] = {  // turned to look at YOU
    {0,0,3,0,0,0},
    {0,1,1,1,1,0},
    {1,2,1,1,2,1},            // two eyes, head a pixel wider — he's facing out
    {0,1,1,1,1,0},
    {0,4,0,0,4,0},
};

struct PipState {
    float    x = 0;           // sprite left edge, canvas px
    int16_t  y = 0;           // sprite top row
    int8_t   dir = 1;
    uint16_t tick = 0;
    uint8_t  special = 0;     // 0 plain (blink pause), 1 face-the-viewer, 2 sleepy, 3 startled
    int16_t  pauseAt = -1000; // x where he stops (-1000 = no stop scheduled)
    int16_t  pauseTicks = 0;  // >0 = currently paused
};
static PipState pip;

static void pipStartWalk(int rectX, int rectY, int rectW, int rectH) {
    pip = PipState();
    pip.dir = (esp_random() & 1) ? 1 : -1;
    pip.x   = pip.dir > 0 ? rectX - PIP_W : rectX + rectW;
    pip.y   = rectY + rectH - PIP_H;
    uint32_t roll = esp_random() % 100;
    pip.special = roll < 60 ? 0 : (roll < 82 ? 1 : 2);
    // stop somewhere near the middle (every walk pauses — that's when he blinks)
    pip.pauseAt = rectX + rectW / 2 - PIP_W / 2 + (int)(esp_random() % 5) - 2;
    pip.tick = esp_random() % 4;
}

// Advance one animation tick. Returns false once he has walked off the edge.
static bool pipAdvance(int leftBound, int rightBound) {
    pip.tick++;
    if (pip.pauseTicks > 0) {
        pip.pauseTicks--;
        if (pip.pauseTicks == 0 && pip.special == 2)
            pip.special = 3;   // woke up with a start — scurries off at 2x
        return true;
    }
    pip.x += pip.dir * (pip.special == 3 ? 1.0f : 0.5f);
    if (pip.pauseAt > -999 &&
        ((pip.dir > 0 && pip.x >= pip.pauseAt) || (pip.dir < 0 && pip.x <= pip.pauseAt))) {
        pip.pauseTicks = pip.special == 2 ? 34 : (pip.special == 1 ? 16 : 10);
        pip.pauseAt = -1000;
    }
    return !((pip.dir > 0 && pip.x > rightBound) ||
             (pip.dir < 0 && pip.x < leftBound - PIP_W));
}

static void pipDraw(Renderer& r) {
    bool paused = pip.pauseTicks > 0;
    bool flip   = pip.dir < 0;
    const uint8_t (*spr)[PIP_W] =
        paused ? (pip.special == 1 ? PIP_FACE : PIP_WALK[0])
               : PIP_WALK[(pip.tick >> 1) & 1];
    // eyes shut while sleeping, and briefly (a blink) during plain pauses
    bool eyesClosed = paused && (pip.special == 2 ||
                                 (pip.special == 0 && (pip.pauseTicks % 9) < 2));
    int x = (int)pip.x, y = pip.y;
    for (int row = 0; row < PIP_H; row++)
        for (int col = 0; col < PIP_W; col++) {
            uint8_t c = spr[row][flip ? PIP_W - 1 - col : col];
            if (!c) continue;
            uint8_t v = c == 1 ? 190
                      : c == 2 ? (eyesClosed ? 190 : 0)
                      : c == 3 ? 255 : 165;
            r.setPixel(x + col, y + row, v);
        }
    if (paused && pip.special == 2) {
        // z's drifting up from his head
        int zn = (pip.tick / 3) % 4;
        for (int i = 0; i < zn; i++)
            r.setPixel(x + (flip ? 1 - i : 4 + i), y - 1 - i, (uint8_t)(110 + i * 40));
    }
}

// ---------------------------------------------------------------------------
// Snake helpers
// ---------------------------------------------------------------------------

static bool snakeOccupied(int x, int y) {
    for (int i = 0; i < snakeLen; i++) {
        int idx = (snakeHead - i + SNAKE_MAX) % SNAKE_MAX;
        if (snakeX[idx] == x && snakeY[idx] == y) return true;
    }
    return false;
}

static void snakeSpawnFood(int W, int H) {
    for (int t = 0; t < 40; t++) {
        int x = esp_random() % W, y = esp_random() % H;
        if (!snakeOccupied(x, y)) { foodX = x; foodY = y; return; }
    }
    foodX = 0; foodY = 0;
}

static void snakeReset(int W, int H) {
    snakeLen  = 3;
    snakeHead = 2;
    int cx = W / 2 - 1, cy = H / 2;
    for (int i = 0; i < 3; i++) { snakeX[i] = cx - 2 + i; snakeY[i] = cy; }
    snakeDX = 1; snakeDY = 0;
    snakeDead = 0;
    snakeSpawnFood(W, H);
}

static void pongServe(int W, int H) {
    pongBX = W * 0.5f;
    pongBY = H * 0.5f;
    pongVX = (esp_random() & 1) ? 0.55f : -0.55f;
    pongVY = ((int)(esp_random() % 100) - 50) * 0.012f;
}

static void warpSpawn(Star& s, int W, int H, bool anywhere) {
    float ang = frand() * 6.2831853f;
    float sp  = 0.06f + frand() * 0.14f;
    float d   = anywhere ? frand() * (W < H ? W : H) * 0.4f : 1.2f;
    s.vx = cosf(ang) * sp;
    s.vy = sinf(ang) * sp;
    s.x  = W * 0.5f + cosf(ang) * d;
    s.y  = H * 0.5f + sinf(ang) * d;
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

Effect effectByName(const char* n) {
    struct { const char* name; Effect e; } static const map[] = {
        { "__sweep__",     Effect::Sweep     }, { "__identify__", Effect::Identify },
        { "__rain__",      Effect::Rain      }, { "__fire__",     Effect::Fire     },
        { "__plasma__",    Effect::Plasma    }, { "__sparkle__",  Effect::Sparkle  },
        { "__life__",      Effect::Life      }, { "__bars__",     Effect::Bars     },
        { "__scope__",     Effect::Scope     }, { "__bounce__",   Effect::Bounce   },
        { "__snake__",     Effect::Snake     }, { "__pong__",     Effect::Pong     },
        { "__blobs__",     Effect::Blobs     }, { "__ripple__",   Effect::Ripple   },
        { "__warp__",      Effect::Warp      }, { "__fireworks__",Effect::Fireworks},
        { "__sand__",      Effect::Sand      }, { "__radar__",    Effect::Radar    },
        { "__heart__",     Effect::Heart     }, { "__disco__",    Effect::Disco    },
        { "__static__",    Effect::Static    }, { "__critter__",  Effect::Critter  },
    };
    for (auto& m : map)
        if (strcmp(n, m.name) == 0) return m.e;
    return Effect::None;
}

void effectsLayoutChanged(Renderer& r) {
    int W = r.width(), H = r.height();
    free(fireHeat);
    fireHeat = (uint8_t*)calloc((size_t)W * (H + 1), 1);
    free(lifeGrid[0]);   // both grids live in one allocation
    lifeGrid[0] = (uint8_t*)calloc((size_t)W * H * 2, 1);
    lifeGrid[1] = lifeGrid[0] ? lifeGrid[0] + (size_t)W * H : nullptr;
}

static void seedLife(Renderer& r) {
    lifeGen = 0;
    if (!lifeGrid[0]) return;
    int W = r.width(), H = r.height();
    memset(lifeGrid[0], 0, (size_t)W * H * 2);
    uint8_t* g = lifeGrid[lifeCur];
    for (int i = 0; i < W * H; i++)
        g[i] = (esp_random() % 100) < 28;   // ~28% starting density
}

void effectInit(Effect e, Renderer& r) {
    int W = r.width(), H = r.height();
    switch (e) {
    case Effect::Sweep:   sweepCol = 0; break;
    case Effect::Rain:    for (int i = 0; i < CANVAS_MAX_W; i++) rainDrop[i] = -1; break;
    case Effect::Fire:    if (fireHeat) memset(fireHeat, 0, (size_t)W * (H + 1)); break;
    case Effect::Plasma:  plasmaT = 0; break;
    case Effect::Life:    seedLife(r); break;
    case Effect::Bars:
        for (int i = 0; i < MAX_BARS; i++) barLvl[i] = barTgt[i] = barPeak[i] = 0;
        break;
    case Effect::Scope:   scopePhase = 0; scopeT = 0; break;
    case Effect::Bounce:
        ballX = 1 + frand() * (W - 4);
        ballY = 1 + frand() * (H - 4);
        ballVX = (esp_random() & 1 ? 1 : -1) * (0.5f + frand() * 0.25f);
        ballVY = (esp_random() & 1 ? 1 : -1) * (0.35f + frand() * 0.3f);
        break;
    case Effect::Snake:   snakeReset(W, H); break;
    case Effect::Pong:
        padL = padR = (H - (H / 4 < 3 ? 3 : H / 4)) * 0.5f;
        pongPause = 0;
        pongServe(W, H);
        break;
    case Effect::Blobs:
        for (int i = 0; i < 3; i++) {
            blobX[i] = 2 + frand() * (W - 4);
            blobY[i] = 2 + frand() * (H - 4);
            blobVX[i] = (esp_random() & 1 ? 1 : -1) * (0.10f + frand() * 0.14f);
            blobVY[i] = (esp_random() & 1 ? 1 : -1) * (0.10f + frand() * 0.14f);
        }
        break;
    case Effect::Ripple:  for (auto& g : rings) g.on = 0; break;
    case Effect::Warp:    for (auto& s : stars) warpSpawn(s, W, H, true); break;
    case Effect::Fireworks:
        for (auto& s : sparks) s.life = 0;
        fwPhase = 0; fwPause = 1;   // launch on the next tick
        break;
    case Effect::Sand:    sandSpout = W * 0.5f; sandFull = 0; break;
    case Effect::Radar:   radarA = 0; for (auto& b : blips) b.on = 0; break;
    case Effect::Heart:   heartTick = 0; break;
    case Effect::Disco:   discoTick = 0; discoPat = 0; break;
    case Effect::Static:  tvRoll = 0; break;
    case Effect::Critter: pipStartWalk(0, 0, W, H - 1); break;   // H-1: he stands on the floor line
    default: break;
    }
}

// ---------------------------------------------------------------------------
// Per-effect ticks
// ---------------------------------------------------------------------------

void effectTick(Effect e, Renderer& r, const AppConfig& cfg,
                uint16_t crX, uint16_t crY, uint16_t crW, uint16_t crH) {
    int W = r.width(), H = r.height();
    effTick++;

    switch (e) {

    case Effect::Sweep:   // one lit column advancing per frame (hardware test)
        r.clear();
        for (int y = 0; y < H; y++) r.setPixel(sweepCol, y, 255);
        sweepCol = (sweepCol + 1) % W;
        break;

    case Effect::Identify: {
        // Layout setup helper: each panel shows its chain index plus a blinking
        // tick on its top edge. Drawn axis-aligned on the canvas, so a wrong rot
        // value shows a sideways digit / misplaced tick.
        r.clear();
        for (uint8_t k = 0; k < cfg.numPanels; k++) {
            const PanelPlacement& p = cfg.layout[k];
            r.drawChar(p.x + 5, p.y + 5, (char)('0' + k), 255);
            if ((millis() / 500) & 1)
                for (int x = 6; x <= 9; x++) r.setPixel(p.x + x, p.y, 255);
        }
        break;
    }

    case Effect::Rain: {  // falling bright heads with fading trails
        fadeCanvas(r, 3, 4);
        for (int c = 0; c < W; c++) {
            if (rainDrop[c] < 0 && (esp_random() % 100) < 12) rainDrop[c] = 0;
            if (rainDrop[c] >= 0) {
                r.setPixel(c, rainDrop[c], 255);
                if (++rainDrop[c] >= H) rainDrop[c] = -1;
            }
        }
        break;
    }

    case Effect::Fire: {  // classic heat-propagation fire, fuel row below the view
        if (!fireHeat) break;
        for (int x = 0; x < W; x++)
            fireHeat[(size_t)H * W + x] = 140 + (esp_random() % 116);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                int below = (y + 1) * W;
                int l = x > 0 ? x - 1 : 0;
                int rr = x < W - 1 ? x + 1 : W - 1;
                int h = (fireHeat[below + x] * 2 + fireHeat[below + l] + fireHeat[below + rr]) / 4;
                int dec = esp_random() % 24;
                fireHeat[y * W + x] = (uint8_t)(h > dec ? h - dec : 0);
            }
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                r.setPixel(x, y, fireHeat[y * W + x]);
        break;
    }

    case Effect::Plasma: {  // three drifting sine fields
        float t = plasmaT++ * 0.10f;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                float v = sinf(x * 0.55f + t) + sinf(y * 0.47f - t * 0.8f)
                        + sinf((x + y) * 0.31f + t * 0.6f);
                int b = (int)(127.5f + v * 42.0f);
                r.setPixel(x, y, (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b)));
            }
        break;
    }

    case Effect::Sparkle: {  // random flashes decaying to black
        fadeCanvas(r, 7, 8);
        int n = 2 + (esp_random() % 4);
        for (int i = 0; i < n; i++)
            r.setPixel(esp_random() % W, esp_random() % H, 255);
        break;
    }

    case Effect::Life: {  // Conway B3/S23, wrapped edges, ghost trails
        if (!lifeGrid[0]) break;
        uint8_t* cur = lifeGrid[lifeCur];
        uint8_t* nxt = lifeGrid[lifeCur ^ 1];
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                int n = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        if (!dx && !dy) continue;
                        n += cur[((y + dy + H) % H) * W + ((x + dx + W) % W)];
                    }
                uint8_t alive = cur[y * W + x];
                nxt[y * W + x] = (n == 3 || (alive && n == 2)) ? 1 : 0;
            }
        lifeCur ^= 1;
        lifeGen++;
        int pop = 0;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                uint8_t a = lifeGrid[lifeCur][y * W + x];
                pop += a;
                uint8_t v = r.getPixel(x, y);
                r.setPixel(x, y, a ? 255 : (uint8_t)(v * 2 / 5));
            }
        bool same = memcmp(lifeGrid[0], lifeGrid[1], (size_t)W * H) == 0;
        if (!pop || same || lifeGen > 500) seedLife(r);
        break;
    }

    case Effect::Bars: {
        // Fake spectrum analyser: 2px bars, 1px gaps, random-walk levels and
        // falling peak-hold dots. Every synth needs one.
        int nb = W / 3;
        if (nb > MAX_BARS) nb = MAX_BARS;
        if (nb < 1) nb = 1;
        r.clear();
        for (int b = 0; b < nb; b++) {
            if ((esp_random() % 100) < 18)
                barTgt[b] = frand() * H;
            barLvl[b] += (barTgt[b] - barLvl[b]) * 0.35f;
            barPeak[b] -= 0.22f;
            if (barPeak[b] < barLvl[b]) barPeak[b] = barLvl[b];
            int x0 = b * 3 + (W - nb * 3 + 1) / 2;   // center the bar field
            int hpx = (int)barLvl[b];
            if (hpx > H) hpx = H;
            for (int y = 0; y < hpx; y++) {
                uint8_t v = (uint8_t)(80 + 175 * y / (H > 1 ? H - 1 : 1));   // hotter up top
                r.setPixel(x0,     H - 1 - y, v);
                r.setPixel(x0 + 1, H - 1 - y, v);
            }
            int py = (int)barPeak[b];
            if (py >= H) py = H - 1;
            if (py > 0) {
                r.setPixel(x0,     H - 1 - py, 255);
                r.setPixel(x0 + 1, H - 1 - py, 255);
            }
        }
        break;
    }

    case Effect::Scope: {
        // Oscilloscope: a sine trace drifting in frequency and amplitude,
        // columns connected like a real scope's beam.
        r.clear();
        scopeT     += 0.045f;
        scopePhase += 0.31f;
        float amp = (H * 0.5f - 1.5f) * (0.35f + 0.6f * (0.5f + 0.5f * sinf(scopeT * 0.9f)));
        float k   = 0.35f + 0.28f * sinf(scopeT * 0.53f);
        float mid = (H - 1) * 0.5f;
        for (int x = 0; x < W; x += 2) r.setPixel(x, (int)(mid + 0.5f), 26);   // faint centerline
        int prevY = -1;
        for (int x = 0; x < W; x++) {
            int y = (int)(mid + amp * sinf(x * k + scopePhase) + 0.5f);
            int a = (prevY < 0) ? y : prevY, b = y;
            if (a > b) { int t = a; a = b; b = t; }
            for (int yy = a; yy <= b; yy++) r.setPixel(x, yy, 255);
            prevY = y;
        }
        break;
    }

    case Effect::Bounce: {  // screensaver ball, 2x2 with a comet trail
        fadeCanvas(r, 3, 4);
        ballX += ballVX;
        ballY += ballVY;
        if (ballX < 0)     { ballX = 0;     ballVX = -ballVX; }
        if (ballX > W - 2) { ballX = W - 2; ballVX = -ballVX; }
        if (ballY < 0)     { ballY = 0;     ballVY = -ballVY; }
        if (ballY > H - 2) { ballY = H - 2; ballVY = -ballVY; }
        int bx = (int)(ballX + 0.5f), by = (int)(ballY + 0.5f);
        for (int dy = 0; dy < 2; dy++)
            for (int dx = 0; dx < 2; dx++)
                r.setPixel(bx + dx, by + dy, 255);
        break;
    }

    case Effect::Snake: {
        // Self-playing snake: greedy chase with a wall/body check. It will
        // eventually trap itself, flash, and respawn — that's half the fun.
        if (snakeDead) {
            snakeDead--;
            r.clear();
            uint8_t v = (snakeDead & 1) ? 255 : 40;
            for (int i = 0; i < snakeLen; i++) {
                int idx = (snakeHead - i + SNAKE_MAX) % SNAKE_MAX;
                r.setPixel(snakeX[idx], snakeY[idx], v);
            }
            if (!snakeDead) snakeReset(W, H);
            break;
        }
        int hx = snakeX[snakeHead], hy = snakeY[snakeHead];
        int fdx = (foodX > hx) - (foodX < hx);
        int fdy = (foodY > hy) - (foodY < hy);
        // score all four directions by progress toward the food, try best first
        static const int8_t DIRS[4][2] = { {1,0}, {-1,0}, {0,1}, {0,-1} };
        int order[4] = { 0, 1, 2, 3 };
        int score[4];
        for (int i = 0; i < 4; i++)
            score[i] = DIRS[i][0] * fdx * 2 + DIRS[i][1] * fdy * 2 + (int)(esp_random() % 2);
        for (int i = 1; i < 4; i++)   // insertion sort, best score first
            for (int j = i; j > 0 && score[order[j]] > score[order[j - 1]]; j--) {
                int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
            }
        int nx = -1, ny = -1;
        for (int i = 0; i < 4; i++) {
            int dx = DIRS[order[i]][0], dy = DIRS[order[i]][1];
            if (dx == -snakeDX && dy == -snakeDY) continue;   // no U-turns
            int tx = hx + dx, ty = hy + dy;
            if (tx < 0 || tx >= W || ty < 0 || ty >= H) continue;
            if (snakeOccupied(tx, ty)) continue;
            nx = tx; ny = ty; snakeDX = dx; snakeDY = dy;
            break;
        }
        if (nx < 0) { snakeDead = 8; break; }   // boxed in
        snakeHead = (snakeHead + 1) % SNAKE_MAX;
        snakeX[snakeHead] = nx;
        snakeY[snakeHead] = ny;
        if (nx == foodX && ny == foodY) {
            if (snakeLen < 46) snakeLen++;
            snakeSpawnFood(W, H);
        }
        r.clear();
        for (int i = snakeLen - 1; i >= 0; i--) {
            int idx = (snakeHead - i + SNAKE_MAX) % SNAKE_MAX;
            uint8_t v = i == 0 ? 255 : (uint8_t)(150 - 90 * i / snakeLen);
            r.setPixel(snakeX[idx], snakeY[idx], v);
        }
        r.setPixel(foodX, foodY, (effTick & 2) ? 255 : 90);
        break;
    }

    case Effect::Pong: {
        // Self-playing pong. The chasing paddle's speed is capped below the
        // ball's, so it genuinely loses points now and then.
        r.clear();
        int ph = H / 4;
        if (ph < 3) ph = 3;
        for (int y = 0; y < H; y += 3) r.setPixel(W / 2, y, 36);   // net
        if (pongPause) {
            if (--pongPause == 0) pongServe(W, H);
        } else {
            pongBX += pongVX;
            pongBY += pongVY;
            if (pongBY < 0)     { pongBY = 0;     pongVY = -pongVY; }
            if (pongBY > H - 1) { pongBY = H - 1; pongVY = -pongVY; }
            float* chase = (pongVX < 0) ? &padL : &padR;
            float* idle  = (pongVX < 0) ? &padR : &padL;
            float diff = (pongBY - ph * 0.5f) - *chase;
            *chase += diff > 0.42f ? 0.42f : (diff < -0.42f ? -0.42f : diff);
            *idle  += ((H - ph) * 0.5f - *idle) * 0.05f;
            if (padL < 0) padL = 0; if (padL > H - ph) padL = H - ph;
            if (padR < 0) padR = 0; if (padR > H - ph) padR = H - ph;
            if (pongBX <= 1 && pongVX < 0) {
                if (pongBY >= padL - 0.5f && pongBY <= padL + ph + 0.5f) {
                    pongVX = -pongVX * 1.03f;
                    if (pongVX > 0.9f) pongVX = 0.9f;
                    pongVY += ((int)(esp_random() % 100) - 50) * 0.008f;
                    pongBX = 1;
                } else if (pongBX < -2) pongPause = 14;   // point lost
            }
            if (pongBX >= W - 2 && pongVX > 0) {
                if (pongBY >= padR - 0.5f && pongBY <= padR + ph + 0.5f) {
                    pongVX = -pongVX * 1.03f;
                    if (pongVX < -0.9f) pongVX = -0.9f;
                    pongVY += ((int)(esp_random() % 100) - 50) * 0.008f;
                    pongBX = W - 2;
                } else if (pongBX > W + 1) pongPause = 14;
            }
        }
        for (int y = 0; y < ph; y++) {
            r.setPixel(0,     (int)padL + y, 255);
            r.setPixel(W - 1, (int)padR + y, 255);
        }
        if (!pongPause) r.setPixel((int)(pongBX + 0.5f), (int)(pongBY + 0.5f), 255);
        break;
    }

    case Effect::Blobs: {
        // Metaball lava lamp — soft grayscale goo, the 8-bit depth's showpiece.
        for (int i = 0; i < 3; i++) {
            blobX[i] += blobVX[i];
            blobY[i] += blobVY[i];
            if (blobX[i] < 1 || blobX[i] > W - 2) blobVX[i] = -blobVX[i];
            if (blobY[i] < 1 || blobY[i] > H - 2) blobVY[i] = -blobVY[i];
        }
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                float f = 0;
                for (int i = 0; i < 3; i++) {
                    float dx = x - blobX[i], dy = y - blobY[i];
                    f += blobR2[i] / (dx * dx + dy * dy + 0.8f);
                }
                int v = (int)((f - 0.75f) * 200.0f);
                r.setPixel(x, y, (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)));
            }
        break;
    }

    case Effect::Ripple: {
        // Raindrops on water: expanding rings that fade as they grow.
        if ((esp_random() % 100) < 9) {
            for (auto& g : rings)
                if (!g.on) {
                    g.on = 1; g.r = 0.5f; g.str = 255;
                    g.cx = esp_random() % W; g.cy = esp_random() % H;
                    break;
                }
        }
        for (auto& g : rings)
            if (g.on) {
                g.r += 0.55f;
                g.str -= 9;
                if (g.str < 20) g.on = 0;
            }
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                int acc = 0;
                for (auto& g : rings) {
                    if (!g.on) continue;
                    float dx = x - g.cx, dy = y - g.cy;
                    float e = fabsf(sqrtf(dx * dx + dy * dy) - g.r);
                    if (e < 1.2f) acc += (int)(g.str * (1.2f - e) / 1.2f);
                }
                r.setPixel(x, y, (uint8_t)(acc > 255 ? 255 : acc));
            }
        break;
    }

    case Effect::Warp: {  // starfield accelerating outward, short streaks
        fadeCanvas(r, 1, 2);
        for (auto& s : stars) {
            s.x += s.vx; s.y += s.vy;
            s.vx *= 1.07f; s.vy *= 1.07f;
            if (s.x < 0 || s.x >= W || s.y < 0 || s.y >= H) { warpSpawn(s, W, H, false); continue; }
            float sp = fabsf(s.vx) + fabsf(s.vy);
            int v = 80 + (int)(sp * 300.0f);
            r.setPixel((int)s.x, (int)s.y, (uint8_t)(v > 255 ? 255 : v));
        }
        break;
    }

    case Effect::Fireworks: {
        fadeCanvas(r, 7, 10);
        if (fwPause) {
            if (--fwPause == 0) {
                rocketX  = 2 + esp_random() % (W > 4 ? W - 4 : 1);
                rocketY  = H - 1;
                rocketVY = -(0.45f + H * 0.014f);
                fwPhase  = 0;
            }
            break;
        }
        if (fwPhase == 0) {   // climb
            rocketY  += rocketVY;
            rocketVY += 0.03f;   // gravity slows the climb
            r.setPixel((int)rocketX, (int)rocketY, 255);
            if (rocketVY > -0.12f) {   // apex — burst
                fwPhase = 1;
                for (int i = 0; i < 28; i++) {
                    float ang = 6.2831853f * i / 28 + frand() * 0.2f;
                    float sp  = 0.3f + frand() * 0.55f;
                    sparks[i] = { rocketX, rocketY, cosf(ang) * sp, sinf(ang) * sp,
                                  (int16_t)(220 + (int)(esp_random() % 36)) };
                }
            }
        } else {
            int alive = 0;
            for (auto& s : sparks) {
                if (s.life <= 0) continue;
                s.x += s.vx; s.y += s.vy;
                s.vy += 0.045f;   // gravity
                s.life -= 7 + esp_random() % 5;
                if (s.y >= H || s.life <= 0) { s.life = 0; continue; }
                r.setPixel((int)s.x, (int)s.y, (uint8_t)(s.life > 255 ? 255 : s.life));
                alive++;
            }
            if (!alive) fwPause = 8 + esp_random() % 18;
        }
        break;
    }

    case Effect::Sand: {
        // Falling sand from a slowly wandering spout; the canvas IS the pile.
        // When the pile reaches the spout, everything pours away and restarts.
        sandSpout += ((float)(esp_random() % 100) - 49.5f) * 0.02f;
        if (sandSpout < 1)     sandSpout = 1;
        if (sandSpout > W - 2) sandSpout = W - 2;
        for (int y = H - 2; y >= 0; y--) {
            bool ltr = (y ^ effTick) & 1;   // alternate scan order — no drift bias
            for (int i = 0; i < W; i++) {
                int x = ltr ? i : W - 1 - i;
                uint8_t v = r.getPixel(x, y);
                if (!v) continue;
                if (!r.getPixel(x, y + 1)) {
                    r.setPixel(x, y + 1, v); r.setPixel(x, y, 0);
                } else {
                    int d = (esp_random() & 1) ? 1 : -1;
                    if (x + d >= 0 && x + d < W && !r.getPixel(x + d, y + 1)) {
                        r.setPixel(x + d, y + 1, v); r.setPixel(x, y, 0);
                    } else if (x - d >= 0 && x - d < W && !r.getPixel(x - d, y + 1)) {
                        r.setPixel(x - d, y + 1, v); r.setPixel(x, y, 0);
                    }
                }
            }
        }
        {
            int sx = (int)sandSpout;
            if (!r.getPixel(sx, 0)) {
                r.setPixel(sx, 0, 120 + esp_random() % 136);   // varied grain = texture
                sandFull = 0;
            } else if (++sandFull > 25) {
                r.clear();
                sandFull = 0;
            }
        }
        break;
    }

    case Effect::Radar: {
        // Rotating beam with phosphor afterglow; blips flare when swept.
        fadeCanvas(r, 13, 16);
        radarA += 0.13f;
        float cx = (W - 1) * 0.5f, cy = (H - 1) * 0.5f;
        float maxR = sqrtf(cx * cx + cy * cy);
        float ca = cosf(radarA), sa = sinf(radarA);
        for (float d = 0; d <= maxR; d += 0.5f)
            r.setPixel((int)(cx + ca * d + 0.5f), (int)(cy + sa * d + 0.5f), 190);
        for (auto& b : blips) {
            if (!b.on || (esp_random() % 300) == 0) {
                b.x = esp_random() % W; b.y = esp_random() % H; b.on = 1;
            }
            float diff = fabsf(remainderf(atan2f(b.y - cy, b.x - cx) - radarA, 6.2831853f));
            if (diff < 0.22f) r.setPixel(b.x, b.y, 255);
        }
        break;
    }

    case Effect::Heart: {
        // Lub-dub. Doesn't get more playful than a heartbeat in your bookshelf.
        r.clear();
        heartTick++;
        int phase = heartTick % 22;
        uint8_t b;
        if      (phase < 5)  b = (uint8_t)(255 - phase * 30);        // LUB
        else if (phase < 7)  b = 105;
        else if (phase < 12) b = (uint8_t)(205 - (phase - 7) * 22);  // dub
        else                 b = 72;
        int x0 = crX + ((int)crW - 16) / 2;   // centered on real panels, never a gap
        int y0 = crY + ((int)crH - 16) / 2;
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++)
                if (HEART_ICON[y] & (0x8000 >> x)) r.setPixel(x0 + x, y0 + y, b);
        break;
    }

    case Effect::Disco: {
        // Geometric party patterns with hard cuts — the Frekvens line was
        // literally a party collab. Includes the classic XOR munching squares.
        discoTick++;
        if (discoTick % 24 == 0) discoPat = esp_random() % 6;
        int t = discoTick;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                bool on = false;
                switch (discoPat) {
                    case 0: on = ((x / 2 + y / 2 + t / 6) & 1); break;              // checker pulse
                    case 1: on = (((x + t) / 3) & 1); break;                        // stripes →
                    case 2: on = (((y + t) / 3) & 1); break;                        // stripes ↓
                    case 3: on = (((abs(x - W / 2) + abs(y - H / 2) + t) / 3) & 1); // diamonds out
                            break;
                    case 4: {                                                       // rings out
                        int dx = x - W / 2, dy = y - H / 2;
                        on = (((dx * dx + dy * dy) / 9 + t / 2) & 1);
                        break;
                    }
                    default: on = (((x ^ y) + t) & 4); break;                       // munching squares
                }
                r.setPixel(x, y, on ? 230 : 0);
            }
        break;
    }

    case Effect::Critter: {  // Pip on demand: endless back-and-forth patrol
        r.clear();
        for (int x = 0; x < W; x++) r.setPixel(x, H - 1, 22);   // faint floor line
        if (!pipAdvance(0, W)) pipStartWalk(0, 0, W, H - 1);    // walked off — next entrance
        else pipDraw(r);
        break;
    }

    case Effect::Static: {  // between channels: biased noise + rolling sync band
        tvRoll = (tvRoll + 1) % (H * 3);
        int band = tvRoll / 3;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                uint8_t v = esp_random() & 0xFF;
                v = (uint8_t)(((uint16_t)v * v) >> 8);   // bias dark, brighter pops
                if (((y + H - band) % H) < 2) v >>= 2;   // the roll
                r.setPixel(x, y, v);
            }
        break;
    }

    default: break;
    }
}

// ---------------------------------------------------------------------------
// Pip as an idle presence: after the display has been calm for a while, he
// occasionally wanders across the bottom of the panels — in FRONT of whatever
// is showing. The content under him is saved before each draw and restored
// after, so a clock or image heals seamlessly behind him. If the content
// re-renders itself mid-walk (contentEpoch changes), the stale restore is
// skipped — the repaint already erased him.
// ---------------------------------------------------------------------------

#define PIP_IDLE_AFTER_MS 180000UL   // display calm this long before he may appear
#define PIP_TRY_EVERY_MS   20000UL   // how often the dice roll happens after that
#define PIP_CHANCE            12     // 1-in-N per roll (~every 4 min once eligible)
#define PIP_COOLDOWN_MS   120000UL   // minimum gap between two visits
#define PIP_TICK_MS          110UL   // animation tick

// Save-under box: sprite plus headroom for the sleepy z's on either side.
#define PIP_SAVE_W 13
#define PIP_SAVE_H 9

static bool     pipIdleActive = false;
static uint8_t  pipSaveBuf[PIP_SAVE_W * PIP_SAVE_H];
static int16_t  pipSaveX = 0, pipSaveY = 0;
static uint32_t pipSaveEpoch = 0;
static bool     pipSaveValid = false;
static uint32_t pipCalmSince = 0;      // 0 = not currently calm
static uint32_t pipNextTry   = 0;
static uint32_t pipLastTick  = 0;

static void pipSaveUnder(Renderer& r, uint32_t epoch) {
    pipSaveX = (int)pip.x - 3;
    pipSaveY = pip.y - 4;
    for (int row = 0; row < PIP_SAVE_H; row++)
        for (int col = 0; col < PIP_SAVE_W; col++)
            pipSaveBuf[row * PIP_SAVE_W + col] = r.getPixel(pipSaveX + col, pipSaveY + row);
    pipSaveEpoch = epoch;
    pipSaveValid = true;
}

static void pipRestoreUnder(Renderer& r, uint32_t epoch) {
    if (!pipSaveValid) return;
    if (epoch == pipSaveEpoch) {   // stale after a content repaint — skip
        for (int row = 0; row < PIP_SAVE_H; row++)
            for (int col = 0; col < PIP_SAVE_W; col++)
                r.setPixel(pipSaveX + col, pipSaveY + row,
                           pipSaveBuf[row * PIP_SAVE_W + col]);
    }
    pipSaveValid = false;
}

bool critterService(Renderer& r, bool calm,
                    uint16_t crX, uint16_t crY, uint16_t crW, uint16_t crH,
                    uint32_t contentEpoch, uint32_t nowMs) {
    if (!calm) {
        // Mode changed or something busy is playing: vanish without touching the
        // canvas (the new content has already repainted it).
        pipIdleActive = false;
        pipSaveValid  = false;
        pipCalmSince  = 0;
        return false;
    }
    if (pipCalmSince == 0) pipCalmSince = nowMs ? nowMs : 1;

    if (!pipIdleActive) {
        if (nowMs - pipCalmSince < PIP_IDLE_AFTER_MS || nowMs < pipNextTry) return false;
        pipNextTry = nowMs + PIP_TRY_EVERY_MS;
        if (esp_random() % PIP_CHANCE) return false;
        pipStartWalk(crX, crY, crW, crH);
        pipIdleActive = true;
        pipSaveValid  = false;
        pipLastTick   = 0;
    }

    if (nowMs - pipLastTick < PIP_TICK_MS) return false;
    pipLastTick = nowMs;

    pipRestoreUnder(r, contentEpoch);
    if (!pipAdvance(crX, crX + crW)) {
        // Walked off the edge — content is already restored; rest a while.
        pipIdleActive = false;
        pipNextTry    = nowMs + PIP_COOLDOWN_MS;
        return true;
    }
    pipSaveUnder(r, contentEpoch);
    pipDraw(r);
    return true;
}

// ---------------------------------------------------------------------------
// Weather ambience — the panel as a tiny window on the sky.
// Each HA condition maps to a hand-designed generative scene. Scenes share a
// particle pool and are tuned for the REAL LED panel, not the web preview:
// the LEDs compress midtones (45 vs 110 vs 205 all read as "lit"), so scenes
// use at most three widely-spaced levels (~40 dim / ~130 mid / 255 full),
// posterized silhouettes instead of gradients, on/off blinking instead of
// gray shimmer, and keep secondary elements (stars, halos) physically clear
// of the scene's main shape so they can't merge into it.
// ---------------------------------------------------------------------------

#define WPART 60
static float    wpX[WPART], wpY[WPART], wpVX[WPART], wpVY[WPART], wpPh[WPART];
static float    wAngle = 0;
static uint16_t wTick  = 0;
static uint16_t wFlash = 0;      // storm: bright-flash countdown
static uint16_t wNextFlash = 0;  // storm: ticks until the next bolt
static uint8_t  boltX[32];       // storm: jagged bolt path (one x per row)
static uint8_t  boltH = 0;

Weather weatherByCondition(const char* c) {
    if (!c) return Weather::Clouds;
    if (!strcmp(c, "sunny") || !strcmp(c, "clear-day"))            return Weather::Sun;
    if (!strcmp(c, "clear-night"))                                 return Weather::Night;
    if (!strcmp(c, "partlycloudy"))                                return Weather::PartlyCloudy;
    if (!strcmp(c, "cloudy") || !strcmp(c, "exceptional"))         return Weather::Clouds;
    if (!strcmp(c, "fog"))                                         return Weather::Fog;
    if (!strcmp(c, "rainy") || !strcmp(c, "pouring"))             return Weather::Rain;
    if (!strcmp(c, "snowy"))                                       return Weather::Snow;
    if (!strcmp(c, "snowy-rainy") || !strcmp(c, "hail"))          return Weather::Sleet;
    if (!strcmp(c, "lightning") || !strcmp(c, "lightning-rainy")) return Weather::Storm;
    if (!strcmp(c, "windy") || !strcmp(c, "windy-variant"))       return Weather::Wind;
    return Weather::Clouds;
}

const char* weatherName(Weather w) {
    switch (w) {
        case Weather::Sun:          return "sun";
        case Weather::Night:        return "night";
        case Weather::PartlyCloudy: return "partly";
        case Weather::Fog:          return "fog";
        case Weather::Rain:         return "rain";
        case Weather::Snow:         return "snow";
        case Weather::Sleet:        return "sleet";
        case Weather::Storm:        return "storm";
        case Weather::Wind:         return "wind";
        default:                    return "clouds";
    }
}

// Night-scene moon placement, shared by init (star exclusion) and tick (draw).
static void nightMoonGeom(int W, int H, float& mx, float& my, float& mr) {
    mx = W * 0.72f; my = H * 0.28f; mr = H * 0.19f + 1.0f;
}
#define NIGHT_STARS 7

void weatherInit(Weather w, Renderer& r) {
    int W = r.width(), H = r.height();
    wTick = 0; wAngle = 0; wFlash = 0; wNextFlash = 25 + esp_random() % 55; boltH = 0;
    for (int i = 0; i < WPART; i++) {
        wpX[i]  = esp_random() % W;
        wpY[i]  = esp_random() % H;
        wpPh[i] = frand() * 6.2831853f;
    }
    switch (w) {
    case Weather::Clouds:
    case Weather::PartlyCloudy:
        for (int i = 0; i < 3; i++) {
            wpX[i]  = frand() * W;
            wpY[i]  = H * 0.34f + frand() * H * 0.22f;
            wpVX[i] = 0.05f + frand() * 0.06f;
        }
        break;
    case Weather::Rain:
    case Weather::Storm:
        for (int i = 0; i < WPART; i++) { wpVX[i] = 0.18f; wpVY[i] = 0.9f + frand() * 0.6f; }
        break;
    case Weather::Snow:
        for (int i = 0; i < WPART; i++) wpVY[i] = 0.22f + frand() * 0.22f;
        break;
    case Weather::Sleet:
        for (int i = 0; i < WPART; i++)
            if (i & 1) { wpVX[i] = 0.28f; wpVY[i] = 1.0f + frand() * 0.45f; }
            else       { wpVX[i] = 0.0f;  wpVY[i] = 0.30f + frand() * 0.20f; }
        break;
    case Weather::Wind:
        for (int i = 0; i < WPART; i++) { wpVX[i] = 0.6f + frand() * 0.6f; wpVY[i] = (frand() - 0.5f) * 0.2f; }
        break;
    case Weather::Night: {
        // Stars must never touch the moon — on the LED panel a star pixel next
        // to the crescent is indistinguishable from part of it. Keep each star
        // well outside the moon disc and a couple of pixels from other stars.
        float mx, my, mr;
        nightMoonGeom(W, H, mx, my, mr);
        for (int i = 0; i < NIGHT_STARS; i++) {
            for (int tries = 0; tries < 40; tries++) {
                wpX[i] = esp_random() % W;
                wpY[i] = esp_random() % H;
                float dx = wpX[i] - mx, dy = wpY[i] - my;
                if (dx * dx + dy * dy <= (mr + 2.0f) * (mr + 2.0f)) continue;
                bool crowded = false;
                for (int j = 0; j < i; j++) {
                    float sx = wpX[i] - wpX[j], sy = wpY[i] - wpY[j];
                    if (sx * sx + sy * sy < 6.0f) { crowded = true; break; }
                }
                if (!crowded) break;
            }
        }
        break;
    }
    default: break;
    }
}

// Rain/storm droplet update shared by both scenes: bright head + dim streak,
// small splash at the floor, wind-blown reset back to the top.
static void weatherRainStep(Renderer& r, int W, int H, int np) {
    for (int i = 0; i < np; i++) {
        wpX[i] += wpVX[i]; wpY[i] += wpVY[i];
        if (wpY[i] >= H) {
            r.setPixel(((int)wpX[i] % W + W) % W, H - 1, 140);   // splash pops as an event
            wpY[i] = 0; wpX[i] = esp_random() % W;
        }
        // Full-bright head over a genuinely faint tail — at 90 the tail read
        // as bright as the head on the LEDs, turning drops into solid bars.
        int px = ((int)wpX[i] % W + W) % W, py = (int)wpY[i];
        if (py - 1 >= 0) r.setPixel(px, py - 1, 45);
        r.setPixel(px, py, 255);
    }
}

void weatherTick(Weather w, Renderer& r) {
    int W = r.width(), H = r.height();
    wTick++;
    int np = W * 3; if (np > WPART) np = WPART;

    switch (w) {

    case Weather::Sun: {
        // Solid full-bright disc, a REAL dark gap, then full-bright rays whose
        // LENGTH breathes. On the LEDs a half-bright halo just smears into the
        // disc, and a brightness pulse is invisible — but shape and motion read
        // perfectly, so the pulse lives in the ray length instead.
        r.clear();
        wAngle += 0.045f;
        float cx = (W - 1) * 0.5f, cy = H * 0.44f;
        float rad = H * 0.17f + 1.0f;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                float dx = x - cx, dy = y - cy;
                if (dx * dx + dy * dy <= rad * rad) r.setPixel(x, y, 255);
            }
        float breathe = 0.5f + 0.5f * sinf(wTick * 0.06f);         // 0..1
        float tip = rad + 2.2f + breathe * 1.6f;                    // rays extend/retract
        for (int k = 0; k < 8; k++) {
            float a = wAngle + k * 0.785398f;
            for (float rr = rad + 1.8f; rr <= tip; rr += 0.5f)      // ≥1px gap after the disc
                r.setPixel((int)(cx + cosf(a) * rr + 0.5f), (int)(cy + sinf(a) * rr + 0.5f), 255);
        }
        break;
    }

    case Weather::Night: {
        // LED-legible night: the crescent is the ONLY full-bright constant
        // shape; stars are few, dim, kept away from it (see weatherInit), and
        // twinkle by blinking fully OFF and ON — a temporal cue the LEDs
        // render faithfully, unlike a gray-level shimmer.
        r.clear();
        float mx, my, mr;
        nightMoonGeom(W, H, mx, my, mr);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                float dx = x - mx, dy = y - my;
                if (dx * dx + dy * dy <= mr * mr) {
                    float sx = x - (mx - mr * 0.75f);
                    if (sx * sx + dy * dy > mr * mr * 0.95f) r.setPixel(x, y, 255);
                }
            }
        for (int i = 0; i < NIGHT_STARS; i++) {
            float tw = sinf(wTick * 0.05f + wpPh[i]);
            if (tw > 0.1f)   // off ~half the time; a brief full-bright glint at the peak
                r.setPixel((int)wpX[i], (int)wpY[i], tw > 0.92f ? 255 : 70);
        }
        break;
    }

    case Weather::PartlyCloudy: {
        r.clear();
        float cx = W * 0.30f, cy = H * 0.34f, rad = H * 0.14f + 1.0f;   // small static sun
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                float dx = x - cx, dy = y - cy;
                if (dx * dx + dy * dy <= rad * rad) r.setPixel(x, y, 235);
            }
        for (int i = 0; i < 2; i++) { wpX[i] += wpVX[i]; if (wpX[i] > W + 5) wpX[i] = -5; }
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                float f = 0;
                for (int i = 0; i < 2; i++) {
                    float dx = x - wpX[i], dy = y - wpY[i];
                    f += (H * 0.42f) / (dx * dx + dy * dy * 1.7f + 1.0f);
                }
                // Posterize: flat mid core + flat dim rim. A smooth gradient
                // turns to mush on the LEDs; two clearly separated tones keep
                // the cloud a readable blob in front of the bright sun.
                int v = (int)((f - 0.32f) * 190);
                if (v > 120)     r.setPixel(x, y, 130);   // cloud core (over the sun)
                else if (v > 25) r.setPixel(x, y, 40);    // cloud rim
            }
        break;
    }

    case Weather::Clouds: {
        r.clear();
        for (int i = 0; i < 3; i++) { wpX[i] += wpVX[i]; if (wpX[i] > W + 5) wpX[i] = -5; }
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                float f = 0;
                for (int i = 0; i < 3; i++) {
                    float dx = x - wpX[i], dy = y - wpY[i];
                    f += (H * 0.5f) / (dx * dx + dy * dy * 1.6f + 1.0f);
                }
                // Posterized two-tone clouds (see PartlyCloudy note).
                int v = (int)((f - 0.35f) * 170);
                if (v > 110)     r.setPixel(x, y, 120);   // core
                else if (v > 15) r.setPixel(x, y, 40);    // rim
            }
        break;
    }

    case Weather::Fog: {
        // Two dim tones with genuine dark gaps — a continuous gray wash has no
        // visible structure on the LEDs; quantized drifting patches do.
        float t = wTick * 0.05f;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                float n = sinf(x * 0.4f + t) + sinf(y * 0.7f - t * 0.6f) + sinf((x + y) * 0.2f + t * 0.4f);
                if (n > 1.0f)       r.setPixel(x, y, 70);   // dense patch
                else if (n > -0.6f) r.setPixel(x, y, 28);   // thin haze
                else                r.setPixel(x, y, 0);    // clear gap
            }
        break;
    }

    case Weather::Rain:
        fadeCanvas(r, 1, 2);
        weatherRainStep(r, W, H, np);
        break;

    case Weather::Snow: {
        r.clear();
        for (int i = 0; i < np; i++) {
            wpY[i] += wpVY[i];
            if (wpY[i] >= H) { wpY[i] = 0; wpX[i] = esp_random() % W; }
            float sx = wpX[i] + sinf(wTick * 0.05f + wpPh[i]) * 1.5f;   // sway
            int px = (((int)sx) % W + W) % W;
            // Two depth tiers tied to fall speed (fast=near=bright, slow=far=
            // dim) — three near-identical whites gave no parallax on the LEDs.
            r.setPixel(px, (int)wpY[i], wpVY[i] > 0.33f ? 255 : 110);
        }
        break;
    }

    case Weather::Sleet: {
        r.clear();
        for (int i = 0; i < np; i++) {
            wpX[i] += wpVX[i]; wpY[i] += wpVY[i];
            if (wpY[i] >= H) { wpY[i] = 0; wpX[i] = esp_random() % W; }
            int px = (((int)wpX[i]) % W + W) % W;
            if (i & 1) { if ((int)wpY[i] - 1 >= 0) r.setPixel(px, (int)wpY[i] - 1, 45);
                         r.setPixel(px, (int)wpY[i], 255); }        // sleet streak
            else       r.setPixel(px, (int)wpY[i], 120);            // slow flake, clearly dimmer
        }
        break;
    }

    case Weather::Storm: {
        fadeCanvas(r, 1, 2);
        weatherRainStep(r, W, H, np);
        if (wFlash > 0) {
            wFlash--;
            for (int y = 0; y < H; y++)                      // sky-wide flash
                for (int x = 0; x < W; x++) {
                    uint8_t v = r.getPixel(x, y);
                    if (v < 150) r.setPixel(x, y, 150);
                }
            for (int y = 0; y < boltH; y++) {                // the bolt
                r.setPixel(boltX[y], y, 255);
                if (boltX[y] + 1 < W) r.setPixel(boltX[y] + 1, y, 100);  // faint edge, not a 2px-wide bar
            }
        } else if (wNextFlash == 0) {
            wFlash = 2 + esp_random() % 2;                   // strike: build a jagged bolt
            wNextFlash = 25 + esp_random() % 70;
            boltH = H;
            int bx = 2 + esp_random() % (W > 4 ? W - 4 : 1);
            for (int y = 0; y < H; y++) {
                boltX[y] = bx;
                bx += (int)(esp_random() % 3) - 1;
                if (bx < 0) bx = 0; if (bx >= W) bx = W - 1;
            }
        } else {
            wNextFlash--;
        }
        break;
    }

    case Weather::Wind: {
        fadeCanvas(r, 1, 2);
        for (int i = 0; i < np; i++) {
            wpX[i] += wpVX[i]; wpY[i] += wpVY[i];
            if (wpX[i] >= W || wpY[i] < 0 || wpY[i] >= H) {
                wpX[i] = 0; wpY[i] = esp_random() % H;
                wpVX[i] = 0.6f + frand() * 0.6f; wpVY[i] = (frand() - 0.5f) * 0.2f;
            }
            int px = (int)wpX[i], py = (int)wpY[i];
            r.setPixel(px, py, (i % 5 == 0) ? 255 : 100);        // occasional bright "leaf"
            if (px - 1 >= 0) r.setPixel(px - 1, py, 35);          // motion streak
        }
        break;
    }

    default: break;
    }
}
