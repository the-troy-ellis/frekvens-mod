#pragma once
#include <stdint.h>
#include "renderer.h"
#include "storage.h"

// Built-in generative effects, selected via the Animation command with a
// reserved "__name__" (same convention as the __sweep__ hardware test).
// anim.frameMs sets the tick rate. All effects render full-intensity levels
// into the canvas; global brightness stays on the OE PWM.
enum class Effect : uint8_t {
    None,
    Sweep, Identify,                                   // test / setup helpers
    Rain, Fire, Plasma, Sparkle, Life,                 // the classics
    Bars, Scope, Bounce, Snake, Pong, Blobs, Ripple,   // the fun ones
    Warp, Fireworks, Sand, Radar, Heart, Disco, Static,
    Critter,                                           // Pip, on demand
};

// "__rain__" → Effect::Rain, etc. Unknown names → Effect::None (the caller
// falls through to file playback — that's how __preview__.anim still plays).
Effect effectByName(const char* name);

// (Re)allocate the canvas-sized scratch buffers (fire heat, life grids).
// Call whenever the renderer/layout changes, before any effect ticks.
void effectsLayoutChanged(Renderer& r);

// Reset per-effect state (ball positions, snake, spark pools, ...) when an
// effect is selected. The canvas has already been cleared by the caller.
void effectInit(Effect e, Renderer& r);

// Render one tick. cfg supplies the panel layout (__identify__ draws per-panel
// markers); cr* is the largest contiguous panel rectangle for centered content
// (the heart), so it never lands in an arrangement gap.
void effectTick(Effect e, Renderer& r, const AppConfig& cfg,
                uint16_t crX, uint16_t crY, uint16_t crW, uint16_t crH);

// Pip, the idle critter: call every loop() pass. While `calm` is true (clock /
// static image / static text on screen) he occasionally wanders across the
// bottom of the panel rectangle, save-under-restoring the content behind him.
// contentEpoch must increment whenever content re-renders the canvas — a stale
// epoch tells the restore step the content already repainted over him.
// Returns true when the canvas changed (caller sets its dirty flag).
bool critterService(Renderer& r, bool calm,
                    uint16_t crX, uint16_t crY, uint16_t crW, uint16_t crH,
                    uint32_t contentEpoch, uint32_t nowMs);

// Weather ambience — the panel as a "tiny window". A condition string (Home
// Assistant's weather.* state values) selects a hand-designed generative scene.
enum class Weather : uint8_t {
    Sun, Night, PartlyCloudy, Clouds, Fog, Rain, Snow, Sleet, Storm, Wind,
};
// Maps an HA condition string ("rainy", "snowy", "clear-night", ...) to a scene.
// Unknown → Clouds. See the switch in effects.cpp for the full table.
Weather weatherByCondition(const char* haCondition);
// The scene's own short label ("rain", "snow", ...) — used in the WS state echo.
const char* weatherName(Weather w);
void weatherInit(Weather w, Renderer& r);   // reseed particles for the scene
void weatherTick(Weather w, Renderer& r);   // render one ~70 ms ambience frame
