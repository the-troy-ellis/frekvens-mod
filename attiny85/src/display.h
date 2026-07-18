#pragma once
#include <stdint.h>
#include "protocol.h"

// Frekvens panel pins (bit-banged)
#define PIN_LATCH   PB0
#define PIN_CLK     PB1
#define PIN_DATA    PB2

void display_init();

// Called from main loop — cycles bit-planes for PWM brightness
// Must be called as fast as possible (~every 100-200µs)
void display_scan();

// Get pointer to back buffer for filling
uint8_t* display_back_buffer();

// Swap front/back buffers atomically
void display_swap();

// Fill back buffer with zero
void display_clear_back();

// Set global brightness scale (0=off, 255=full)
void display_set_brightness(uint8_t b);
