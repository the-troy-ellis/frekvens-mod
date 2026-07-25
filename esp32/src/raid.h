#pragma once
#include <stdint.h>
#include <stddef.h>   // size_t
class Renderer;

// Raid 16 (docs/raid16.md) — the M0.5 boss showcase (all five boss anatomies
// with their signature animations, driven from raid.html) plus the M1 fight
// engine: a real, winnable VANTA fight with the weighted-deck telegraph
// engine, phases, moods, lobby, and win/lose/stats. Registered in the games
// registry (games.cpp); raid.html is the deck frontend.
//
// Showcase keys (until 'G' enters the fight flow):
//   'V' 'M' 'C' 'B' 'N'  — select boss (Vanta/Moth/Chorus/Bulwark/Null)
//   '0'..'9'             — select an animation within the boss's set
//   'R'/'A'/'U' , 'L'/'D'— next / previous animation
// Auto-tours the current boss's animation set until a manual selection.
// Fight-flow keys are documented at the engine in raid.cpp.

void raidInit(Renderer& r);
void raidInput(uint8_t player, char key, bool pressed);
bool raidTick(Renderer& r, uint32_t nowMs);
// ~10 Hz device→deck JSON snapshot. Returns 0 while the showcase is idle (no
// net traffic outside the fight flow) and also on a too-small buffer — see the
// GAME_NET_BUF contract in games.h, which callers must honour.
size_t raidNet(char* buf, size_t cap);
