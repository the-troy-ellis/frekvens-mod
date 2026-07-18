#pragma once
#include <stdint.h>
class Renderer;

// Raid 16 (docs/raid16.md) — currently the M0.5 boss showcase: all five boss
// anatomies with their signature animations, driven from raid.html. These
// anatomy programs are the real building blocks of the M1 fight engine.
// Registered in the games registry (games.cpp) purely for discovery/start.
//
// Input keys:
//   'V' 'M' 'C' 'B' 'N'  — select boss (Vanta/Moth/Chorus/Bulwark/Null)
//   '0'..'9'             — select an animation within the boss's set
//   'R'/'A'/'U' , 'L'/'D'— next / previous animation
// Auto-tours the current boss's animation set until a manual selection.

void raidInit(Renderer& r);
void raidInput(uint8_t player, char key, bool pressed);
bool raidTick(Renderer& r, uint32_t nowMs);
