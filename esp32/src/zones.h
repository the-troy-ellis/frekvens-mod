#pragma once
#include <stdint.h>
#include "renderer.h"
#include "storage.h"

// Zone mode: each physical panel (chain position) runs its OWN content
// independently — e.g. clock on panel 0, Game of Life on panel 1, Pong on
// panel 2, weather on panel 3 — instead of one mode driving the whole canvas.
// A zone is always exactly one native 16x16 panel; main.cpp composites every
// zone's own small Renderer into the shared canvas each tick via
// Renderer::blitPanelZone, so the existing chain-flush and WS-preview code
// downstream is completely unchanged by zone mode.
//
// SCOPE / KNOWN LIMITATION: Weather, Effect, and Game zone content each reuse
// ONE shared engine instance (effects.cpp / games.cpp keep their animation
// state in file-static singletons, not per-zone) — so only ONE zone at a time
// may run each of those families. AppConfig::normalizeZones() enforces this
// at the config layer (a second Weather/Effect/Game assignment is forced back
// to Off before it's ever applied), so zones.cpp never has to arbitrate the
// ambiguous case at runtime. Clock/Image/Text zones have no such limit — any
// number of panels can run them simultaneously.
enum class ZoneMode : uint8_t { Off, Clock, Weather, Effect, Game, Image, Text };
// NOTE: these numeric values are load-bearing — AppConfig::normalizeZones()
// (storage.h) compares raw uint8_t mode values (2/3/4) to avoid a header
// dependency loop. Do not reorder this enum without updating that function.

const char* zoneModeName(ZoneMode m);       // "off","clock","weather","effect","game","image","text"
ZoneMode    zoneModeByName(const char* s);

// Effect/game names accepted by zone Effect/Game slots — a small curated
// subset of the full effects/games registries (kept small deliberately: only
// one Effect zone and one Game zone can ever be active at a time, so this is
// mainly here so the admin UI can offer a dropdown).
uint8_t     zoneEffectCount();
const char* zoneEffectName(uint8_t i);   // "life","fire","plasma","sparkle","rain"

// Allocate the MAX_PANELS persistent 16x16 zone renderers. Call once from
// setup(), after Storage::loadConfig. cfgPtr is retained (not copied) — used
// by Effect-zone ticks, which need an AppConfig& for effectTick's signature.
void zonesInit(AppConfig* cfgPtr);

// (Re)start a zone's content. mode/sub come from AppConfig::zoneSlots[slot]
// (sub means: weather condition or "auto" for Weather, effect name for
// Effect, game name for Game, filename for Image, literal text for Text,
// unused for Off/Clock). Safe to call whether or not zone mode is currently
// on — main.cpp only composites zone output into the canvas while it is.
void zoneApply(uint8_t slot, ZoneMode mode, const char* sub);

// Advance + render one zone. Each zone paces itself internally (Clock ~1/s,
// Weather ~70ms, Effect ~80ms, Game self-paced via gameTick; Image/Text are
// one-shot, rendered once by zoneApply). Returns true if that zone's canvas
// changed this call.
bool zoneTick(uint8_t slot, uint32_t nowMs);

// The zone's own small persistent canvas — main.cpp composites this into the
// shared canvas via renderer->blitPanelZone(slot, zoneCanvas(slot).canvas()).
const Renderer& zoneCanvas(uint8_t slot);

ZoneMode    zoneCurrentMode(uint8_t slot);
const char* zoneCurrentSub(uint8_t slot);

// Controller input from the play page, addressed to a specific zone. No-op
// unless that zone is currently running a Game.
void zoneInputRoute(uint8_t slot, uint8_t player, char key, bool pressed);
