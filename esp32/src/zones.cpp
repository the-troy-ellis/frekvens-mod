#include "zones.h"
#include "effects.h"
#include "games.h"
#include "config.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <string.h>
#include <time.h>

// --- Name tables ---

static const char* MODE_NAMES[] = { "off", "clock", "weather", "effect", "game", "image", "text" };

const char* zoneModeName(ZoneMode m) {
    uint8_t i = (uint8_t)m;
    return i < 7 ? MODE_NAMES[i] : "off";
}
ZoneMode zoneModeByName(const char* s) {
    if (!s) return ZoneMode::Off;
    for (uint8_t i = 0; i < 7; i++)
        if (strcmp(s, MODE_NAMES[i]) == 0) return (ZoneMode)i;
    return ZoneMode::Off;
}

// Curated subset of effects.cpp's registry — deliberately small (see zones.h:
// only one Effect zone can ever be active, so this exists for the admin UI's
// dropdown, not as a general-purpose list). Each maps to the SAME "__name__"
// wrapping the whole-canvas Animation command uses, so effectByName() (the
// existing, already-verified name table in effects.cpp) does the real lookup
// — no separate/duplicated Effect table to keep in sync here.
static const char* ZONE_EFFECTS[] = { "life", "fire", "plasma", "sparkle", "rain" };
#define N_ZONE_EFFECTS (sizeof(ZONE_EFFECTS) / sizeof(ZONE_EFFECTS[0]))

uint8_t     zoneEffectCount()        { return N_ZONE_EFFECTS; }
const char* zoneEffectName(uint8_t i) { return i < N_ZONE_EFFECTS ? ZONE_EFFECTS[i] : ""; }

static Effect zoneEffectFromSub(const char* sub) {
    for (uint8_t i = 0; i < N_ZONE_EFFECTS; i++)
        if (strcmp(sub, ZONE_EFFECTS[i]) == 0) {
            char wrapped[28];
            snprintf(wrapped, sizeof(wrapped), "__%s__", sub);
            return effectByName(wrapped);
        }
    return Effect::None;   // not in the curated set — stays blank rather than
                            // reaching into the full 20-effect registry
}

// --- Per-zone runtime state ---

struct ZoneRuntime {
    ZoneMode       mode = ZoneMode::Off;
    char           sub[24] = {};
    Renderer*      r = nullptr;         // persistent 16x16 canvas for this zone
    uint32_t       lastTick = 0;        // Weather/Effect pacing (they don't self-pace)
    int            lastMin = -1;        // Clock redraw detection
    bool           blinkPrev = false;
    Weather        weatherScene = Weather::Clouds;
    Effect         effectKind = Effect::None;
};
static ZoneRuntime zr[MAX_PANELS];
static AppConfig* g_cfg = nullptr;

void zonesInit(AppConfig* cfgPtr) {
    g_cfg = cfgPtr;
    for (uint8_t i = 0; i < MAX_PANELS; i++) {
        if (!zr[i].r) zr[i].r = new Renderer(PANEL_WIDTH, PANEL_HEIGHT, nullptr, 1);
    }
}

// Zone clock: always the compact "HH over MM" 16x16 layout (a zone is always
// exactly one native panel — there's never the extra width the whole-canvas
// clock uses for a single-line HH:MM), always 24-hour (keeps the zone content
// menu simple; the whole-canvas Clock mode already covers 12-hour). "--:--"
// until SNTP syncs, matching the whole-canvas clock's convention.
static void zoneRenderClock(Renderer& r, const struct tm& tmv, bool synced, bool blinkOn) {
    r.clear();
    char d[4] = {'-', '-', '-', '-'};
    if (synced) {
        d[0] = (char)('0' + tmv.tm_hour / 10);
        d[1] = (char)('0' + tmv.tm_hour % 10);
        d[2] = (char)('0' + tmv.tm_min / 10);
        d[3] = (char)('0' + tmv.tm_min % 10);
    }
    r.drawChar(2, 0, d[0], 255);
    r.drawChar(8, 0, d[1], 255);
    r.drawChar(2, 9, d[2], 255);
    r.drawChar(8, 9, d[3], 255);
    if (blinkOn) {
        r.setPixel(7, 7, 255); r.setPixel(8, 7, 255);
        r.setPixel(7, 8, 255); r.setPixel(8, 8, 255);
    }
}

void zoneApply(uint8_t slot, ZoneMode mode, const char* sub) {
    if (slot >= MAX_PANELS || !zr[slot].r) return;
    ZoneRuntime& z = zr[slot];
    z.mode = mode;
    strncpy(z.sub, sub ? sub : "", sizeof(z.sub) - 1);
    z.lastTick = 0; z.lastMin = -1; z.blinkPrev = false;
    z.r->clear();

    switch (mode) {
    case ZoneMode::Weather:
        z.weatherScene = weatherByCondition(z.sub);
        weatherInit(z.weatherScene, *z.r);
        break;
    case ZoneMode::Effect:
        z.effectKind = zoneEffectFromSub(z.sub);
        if (z.effectKind != Effect::None) effectInit(z.effectKind, *z.r);
        break;
    case ZoneMode::Game:
        gameStart(z.sub, *z.r);
        break;
    case ZoneMode::Image: {
        // Zone images must be exactly one panel (16x16=256 bytes) — a zone IS
        // one physical panel, there's no bounding-box canvas to scale into.
        char path[80];
        snprintf(path, sizeof(path), "%s/%s", IMAGE_DIR, z.sub);
        File f = LittleFS.open(path, "r");
        if (f) {
            uint8_t buf[PANEL_WIDTH * PANEL_HEIGHT];
            size_t got = f.read(buf, sizeof(buf));
            if (got == sizeof(buf)) z.r->blitImage(0, 0, buf, PANEL_WIDTH, PANEL_HEIGHT);
            f.close();
        }
        break;
    }
    case ZoneMode::Text: {
        // Rendered once, centered — no scrolling in zone mode (v1 scope).
        int len = (int)strlen(z.sub);
        if (len > 0) {
            if (len > 2) len = 2;   // 2 chars (12px) is all that fits legibly at 16px wide
            int w = len * 6 - 1;
            int x0 = (PANEL_WIDTH - w) / 2, y0 = (PANEL_HEIGHT - 7) / 2;
            for (int i = 0; i < len; i++) z.r->drawChar(x0 + i * 6, y0, z.sub[i], 255);
        }
        break;
    }
    default: break;   // Off / Clock: nothing to seed, Clock redraws every tick
    }
}

bool zoneTick(uint8_t slot, uint32_t nowMs) {
    if (slot >= MAX_PANELS || !zr[slot].r) return false;
    ZoneRuntime& z = zr[slot];

    switch (z.mode) {
    case ZoneMode::Clock: {
        bool blink = (nowMs / 1000) & 1;
        time_t tt = time(nullptr);
        struct tm tmv;
        localtime_r(&tt, &tmv);
        bool synced = (tmv.tm_year + 1900) >= 2020;
        if (z.lastMin == tmv.tm_min && z.blinkPrev == blink) return false;
        z.lastMin = tmv.tm_min; z.blinkPrev = blink;
        zoneRenderClock(*z.r, tmv, synced, blink);
        return true;
    }
    case ZoneMode::Weather:
        if (nowMs - z.lastTick < 70) return false;
        z.lastTick = nowMs;
        weatherTick(z.weatherScene, *z.r);
        return true;
    case ZoneMode::Effect:
        if (z.effectKind == Effect::None) return false;
        if (nowMs - z.lastTick < 80) return false;
        z.lastTick = nowMs;
        // crX/Y/W/H are always the whole zone (0,0,16,16) — a zone has no
        // arrangement gaps to avoid; g_cfg is only consulted by __identify__,
        // which isn't in the zone-effect curated set.
        effectTick(z.effectKind, *z.r, *g_cfg, 0, 0, PANEL_WIDTH, PANEL_HEIGHT);
        return true;
    case ZoneMode::Game:
        return gameTick(*z.r, nowMs);   // self-paced
    case ZoneMode::Image:
    case ZoneMode::Text:
    case ZoneMode::Off:
    default:
        return false;   // one-shot (already rendered by zoneApply) or nothing to draw
    }
}

const Renderer& zoneCanvas(uint8_t slot) {
    static Renderer fallback(PANEL_WIDTH, PANEL_HEIGHT, nullptr, 1);
    return (slot < MAX_PANELS && zr[slot].r) ? *zr[slot].r : fallback;
}

ZoneMode zoneCurrentMode(uint8_t slot) {
    return slot < MAX_PANELS ? zr[slot].mode : ZoneMode::Off;
}
const char* zoneCurrentSub(uint8_t slot) {
    return slot < MAX_PANELS ? zr[slot].sub : "";
}

void zoneInputRoute(uint8_t slot, uint8_t player, char key, bool pressed) {
    if (slot >= MAX_PANELS || zr[slot].mode != ZoneMode::Game) return;
    gameInput(player, key, pressed);
}
