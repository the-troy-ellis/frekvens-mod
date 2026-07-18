#pragma once
#include "display.h"
#include "protocol.h"
#include <string.h>

// Boot self-test for a standalone panel (no ESP32 needed).
// Runs continuously until the first frame arrives over UART, then the main
// loop hands off to live mode. Lets you confirm a freshly-built panel works.
//
// Sequence (repeats, ~6 s per cycle):
//   Phase A — smooth fade of ALL pixels 0→full→0.
//             Confirms every LED lights AND that 8-bit brightness PWM works
//             (a stepped or flickery fade points to a scan/timing problem).
//   Phase B — a single lit pixel walks through all 256 shift positions.
//             Confirms every individual LED and the full data/clock/latch chain
//             (a stuck, missing, or doubled dot points to a wiring/SPI fault).
//
// Timing uses loop-iteration counts (millis() is disabled to free the timer).
// One iteration ≈ one display_scan ≈ ~30 µs, so the loop runs ~33 kHz.

static uint8_t  st_phase = 0;   // 0 = fade, 1 = pixel walk
static uint16_t st_ctr   = 0;   // ticks since last sub-step
static uint8_t  st_sub   = 0;   // sub-step index (brightness level or LED index)
static uint8_t  st_dir   = 0;   // fade direction: 0 = up, 1 = down

static void _st_fill_all(uint8_t on) {
    uint8_t* b = display_back_buffer();
    memset(b, on ? 0xFF : 0x00, FRAME_BYTES);
    display_swap();
}

static void _st_one_led(uint8_t k) {
    uint8_t* b = display_back_buffer();
    memset(b, 0, FRAME_BYTES);
    uint8_t byte = k >> 3;          // which byte within a plane
    uint8_t bit  = k & 0x07;        // which bit within that byte
    // Set this bit in all 8 planes → the LED shows at full brightness
    for (uint8_t p = 0; p < BITPLANES; p++)
        b[(uint16_t)p * PLANE_BYTES + byte] = (1 << bit);
    display_swap();
}

static void _st_enter_A() {
    st_phase = 0; st_sub = 0; st_dir = 0; st_ctr = 0;
    _st_fill_all(1);                 // all pixels on…
    display_set_brightness(0);       // …but start dark, fade up
}

static void _st_enter_B() {
    st_phase = 1; st_sub = 0; st_ctr = 0;
    display_set_brightness(255);
    _st_one_led(0);
}

// Begin the self-test. Call once from setup().
static inline void selftest_start() { _st_enter_A(); }

// Advance the self-test by one tick. Call once per main-loop iteration while
// no live frame has been received yet.
static void selftest_tick() {
    st_ctr++;

    if (st_phase == 0) {
        // Phase A — fade all pixels via global brightness
        if (st_ctr >= 150) {                       // ~4.5 ms per brightness step
            st_ctr = 0;
            uint8_t level = (st_dir == 0) ? st_sub : (uint8_t)(255 - st_sub);
            display_set_brightness(level);
            if (st_sub < 255) {
                st_sub++;
            } else {
                st_sub = 0;
                if (st_dir == 0) st_dir = 1;       // reached full → fade back down
                else             _st_enter_B();     // finished fade → pixel walk
            }
        }
    } else {
        // Phase B — walk one lit pixel through all 256 positions
        if (st_ctr >= 500) {                       // ~15 ms per LED
            st_ctr = 0;
            _st_one_led(st_sub);
            if (st_sub < 255) st_sub++;
            else              _st_enter_A();        // finished walk → restart cycle
        }
    }
}

// Restore full brightness when leaving the self-test for live operation.
static inline void selftest_end() { display_set_brightness(255); }
