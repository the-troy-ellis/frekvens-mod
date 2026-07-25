#pragma once
// Minimal host stand-in for the Arduino/ESP runtime, so the pure-logic parts of
// the firmware (raid.cpp) compile and run natively. The test binary supplies
// the definitions, which lets it drive time deterministically and seed the RNG.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>

uint32_t      esp_random();
unsigned long millis();
