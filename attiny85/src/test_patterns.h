#pragma once
#include <stdint.h>
#include "display.h"
#include "protocol.h"

// Startup test: if no valid packet arrives within ~3 seconds, cycle through
// patterns so you can verify wiring before connecting the ESP32.
// Call test_pattern_tick() from the main loop instead of normal receive logic.

static uint8_t  _tp_phase    = 0;
static uint16_t _tp_counter  = 0;
static uint8_t  _tp_col      = 0;

// Build a test frame into the back buffer, plane 0 only (1-bit)
static void _tp_build(uint8_t phase) {
    uint8_t* back = display_back_buffer();
    // Zero all planes
    for (uint8_t i = 0; i < FRAME_BYTES; i++) back[i] = 0;

    uint8_t* plane0 = back;  // first PLANE_BYTES bytes = plane 0

    if (phase == 0) {
        // All pixels off → back buffer already zero
    } else if (phase == 1) {
        // All pixels on (full brightness plane 0)
        for (uint8_t i = 0; i < PLANE_BYTES; i++) plane0[i] = 0xFF;
        // Set remaining planes for full 4-bit brightness (all 4 planes on)
        for (uint8_t p = 1; p < BITPLANES; p++) {
            uint8_t* pp = back + p * PLANE_BYTES;
            for (uint8_t i = 0; i < PLANE_BYTES; i++) pp[i] = 0xFF;
        }
    } else if (phase == 2) {
        // Checkerboard — alternating pixels
        // Pixel (x,y): addr = x + y*8 (x<8) or (x-8) + (y+16)*8 (x>=8)
        // Even-sum pixels on, odd-sum off
        for (uint8_t y = 0; y < 16; y++) {
            for (uint8_t x = 0; x < 16; x++) {
                if ((x + y) % 2 == 0) {
                    uint8_t xa = x, ya = y;
                    if (xa > 7) { ya += 16; xa -= 8; }
                    uint8_t addr = xa + ya * 8;
                    plane0[addr / 8] |= (1 << (addr % 8));
                }
            }
        }
    } else if (phase == 3) {
        // Column sweep — one column at a time
        for (uint8_t y = 0; y < 16; y++) {
            uint8_t x = _tp_col;
            uint8_t xa = x, ya = y;
            if (xa > 7) { ya += 16; xa -= 8; }
            uint8_t addr = xa + ya * 8;
            plane0[addr / 8] |= (1 << (addr % 8));
        }
    }
    display_swap();
}

// Call from main loop during pre-connection test mode.
// Returns 1 once the sequence completes (then normal operation begins).
static uint8_t test_pattern_tick() {
    _tp_counter++;

    // Advance phase every ~1000 scan ticks (~100ms at ~10kHz scan rate)
    if (_tp_counter < 1000) return 0;
    _tp_counter = 0;

    if (_tp_phase == 3) {
        _tp_col++;
        if (_tp_col >= 16) {
            _tp_col = 0;
            _tp_phase = 0;
            return 1;  // full cycle complete
        }
    } else {
        _tp_phase++;
    }

    _tp_build(_tp_phase);
    return 0;
}

static void test_pattern_start() {
    _tp_phase   = 0;
    _tp_counter = 0;
    _tp_col     = 0;
    _tp_build(0);
}
