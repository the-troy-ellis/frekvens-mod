#pragma once
#include <Arduino.h>
#include "config.h"   // MAX_PANELS, PANEL_WIDTH/HEIGHT, CANVAS_MAX_*, PanelPlacement

struct AppConfig {
    // Note: WiFi credentials are owned by WiFiManager (stored in NVS), not here.
    char     mqttBroker[64]   = {};
    uint16_t mqttPort         = 1883;
    char     mqttUser[32]     = {};
    char     mqttPass[32]     = {};
    char     deviceName[32]   = "Frekvens Array";
    uint8_t  numPanels        = 1;
    uint8_t  brightness       = 255;
    bool     mqttEnabled      = false;
    // POSIX TZ string for the clock mode (e.g. "PST8PDT,M3.2.0,M11.1.0" —
    // handles DST automatically). Empty = UTC.
    char     tz[48]           = {};
    // Apply a gamma-2.2 curve to pixel content at the panel (LED PWM is linear,
    // eyes aren't — mid-greys look washed out without it). Off by default so
    // existing images tuned to linear output are unchanged.
    bool     gammaOn          = false;
    // Pip, the idle critter: occasionally wanders across a calm display.
    bool     critterOn        = true;
    // Physical arrangement: layout[k] = where chain position k sits on the
    // logical canvas (see config.h). Default = the original vertical stack.
    PanelPlacement layout[MAX_PANELS];

    // Zone mode: when on, each physical panel runs its OWN content
    // independently (clock on one, a game on another, ...) instead of one
    // mode driving the whole canvas. See zones.h. zoneSlots[k] is chain
    // position k's assignment; mode is a ZoneMode value (stored as uint8_t
    // here to avoid a zones.h dependency in this header).
    bool zoneModeOn = false;
    struct ZoneSlot {
        uint8_t mode;      // ZoneMode — Off(0) by default
        char    sub[24];   // effect/game name, image filename, or text, per mode
    };
    ZoneSlot zoneSlots[MAX_PANELS];

    AppConfig() {
        for (uint8_t i = 0; i < MAX_PANELS; i++) {
            layout[i] = { 0, (int16_t)(i * PANEL_HEIGHT), 0 };
            zoneSlots[i] = { 0, {} };
        }
    }
};

// Zone families that share ONE underlying engine instance (weather/effects/
// games all keep their animation state in a single set of static variables,
// not per-zone — see zones.cpp) can only be assigned to ONE zone at a time;
// a second assignment would silently make both zones mirror/fight over the
// same state. Enforced here so the ambiguous state can never be persisted or
// applied: keeps the first zone with each such mode, forces later duplicates
// back to Off. Modes are compared as raw uint8_t (ZoneMode values 2=Weather,
// 3=Effect, 4=Game — see zones.h) to avoid a header dependency loop.
inline void normalizeZones(AppConfig& cfg) {
    bool seen[8] = {};   // indexed by ZoneMode value, small fixed enum
    for (uint8_t i = 0; i < MAX_PANELS; i++) {
        uint8_t m = cfg.zoneSlots[i].mode;
        if (m >= 8) { cfg.zoneSlots[i].mode = 0; continue; }
        bool exclusive = (m == 2 || m == 3 || m == 4);   // Weather/Effect/Game
        if (exclusive) {
            if (seen[m]) { cfg.zoneSlots[i].mode = 0; continue; }
            seen[m] = true;
        }
    }
}

// Clamp placements into the canvas caps and shift the whole arrangement so its
// bounding box starts at (0,0) — canvas size then follows from layoutBounds().
// Call after any layout mutation (config load, config POST).
inline void normalizeLayout(AppConfig& cfg) {
    if (cfg.numPanels < 1)          cfg.numPanels = 1;
    if (cfg.numPanels > MAX_PANELS) cfg.numPanels = MAX_PANELS;
    int16_t minX = 32767, minY = 32767;
    for (uint8_t i = 0; i < cfg.numPanels; i++) {
        PanelPlacement& p = cfg.layout[i];
        p.rot &= 3;
        if (p.x < 0) p.x = 0;
        if (p.y < 0) p.y = 0;
        if (p.x > CANVAS_MAX_W - PANEL_WIDTH)  p.x = CANVAS_MAX_W - PANEL_WIDTH;
        if (p.y > CANVAS_MAX_H - PANEL_HEIGHT) p.y = CANVAS_MAX_H - PANEL_HEIGHT;
        if (p.x < minX) minX = p.x;
        if (p.y < minY) minY = p.y;
    }
    for (uint8_t i = 0; i < cfg.numPanels; i++) {
        cfg.layout[i].x -= minX;
        cfg.layout[i].y -= minY;
    }
}

// Canvas size = bounding box of the first numPanels placements (assumes
// normalized layout, i.e. min x/y = 0).
inline void layoutBounds(const AppConfig& cfg, uint16_t* w, uint16_t* h) {
    int maxX = PANEL_WIDTH, maxY = PANEL_HEIGHT;
    for (uint8_t i = 0; i < cfg.numPanels && i < MAX_PANELS; i++) {
        if (cfg.layout[i].x + PANEL_WIDTH  > maxX) maxX = cfg.layout[i].x + PANEL_WIDTH;
        if (cfg.layout[i].y + PANEL_HEIGHT > maxY) maxY = cfg.layout[i].y + PANEL_HEIGHT;
    }
    *w = (uint16_t)maxX;
    *h = (uint16_t)maxY;
}

class Storage {
public:
    static bool begin();

    static bool loadConfig(AppConfig& cfg);
    static bool saveConfig(const AppConfig& cfg);

    // List saved image/animation filenames (caller iterates)
    // Returns false if directory doesn't exist
    static bool listImages(String* names, uint8_t maxCount, uint8_t* found);
    static bool listAnims(String* names, uint8_t maxCount, uint8_t* found);

    static bool saveImage(const char* name, const uint8_t* data, size_t len);
    static bool loadImage(const char* name, uint8_t* data, size_t maxLen, size_t* len);
    static bool deleteImage(const char* name);

    static bool saveAnim(const char* name, const uint8_t* data, size_t len);
    static bool loadAnim(const char* name, uint8_t* data, size_t maxLen, size_t* len);
    static bool deleteAnim(const char* name);
};
