#pragma once
#include <stdint.h>
#include "renderer.h"

// Player-controlled games. The device is authoritative: games run here, phones
// are dumb controllers sending input events over the WebSocket
// ({"cmd":"input","p":0,"k":"U","s":1} — player, key U/D/L/R/A, state 1=press).
//
// TO ADD A GAME: write init/input/tick functions in games.cpp and add one row
// to the GAMES[] registry. /api/games serves the registry, so the play page's
// picker updates automatically — no frontend changes needed.

struct GameInfo {
    const char* name;    // wire id, e.g. "snake"
    const char* title;   // display name, e.g. "Snake"
    uint8_t     players; // max controllers that do something
};

uint8_t         gameCount();
const GameInfo& gameInfo(uint8_t i);

// Start a game by wire id (canvas already cleared). False if unknown.
bool gameStart(const char* name, Renderer& r);
// Route a controller event to the running game (call from the loop task only).
void gameInput(uint8_t player, char key, bool pressed);
// Advance/render; games pace themselves off nowMs. True if the canvas changed.
bool gameTick(Renderer& r, uint32_t nowMs);
// Device→deck state channel: if the running game publishes network snapshots
// (Raid 16's ~10 Hz idempotent deck state), copy the latest JSON into buf and
// return its length; 0 = nothing to send / game has no net layer. main.cpp
// polls this at ~10 Hz while a whole-canvas game is active and broadcasts the
// result to every WebSocket client.
size_t gameNetSnapshot(char* buf, size_t cap);
