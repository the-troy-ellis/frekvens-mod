#include "display.h"
#include <avr/io.h>
#include <avr/interrupt.h>
#include <string.h>

// Two complete framebuffers: 4 planes × 32 bytes each = 128 bytes per buffer
static uint8_t buf[2][FRAME_BYTES];
static volatile uint8_t front_idx   = 0;
static volatile uint8_t swap_pending = 0;
static volatile uint8_t brightness  = 255;  // 0=off, 255=full

// Scan state
static uint8_t cur_plane   = 0;
static uint8_t plane_ticks = 0;  // ticks remaining for this plane in current cycle

// All-zero buffer for blank scans during brightness dimming
static const uint8_t blank[PLANE_BYTES] = {};

static inline void pin_high(uint8_t pin) { PORTB |=  (1 << pin); }
static inline void pin_low(uint8_t  pin) { PORTB &= ~(1 << pin); }

static void shift_plane(const uint8_t* plane_data) {
    for (uint8_t i = 0; i < PLANE_BYTES; i++) {
        uint8_t byte = plane_data[i];
        for (uint8_t b = 0; b < 8; b++) {
            if (byte & 0x01) pin_high(PIN_DATA);
            else             pin_low(PIN_DATA);
            byte >>= 1;
            pin_high(PIN_CLK);
            __asm__ __volatile__("nop");
            pin_low(PIN_CLK);
        }
    }
    pin_high(PIN_LATCH);
    __asm__ __volatile__("nop");
    pin_low(PIN_LATCH);
}

void display_init() {
    DDRB |= (1 << PIN_LATCH) | (1 << PIN_CLK) | (1 << PIN_DATA);
    PORTB &= ~((1 << PIN_LATCH) | (1 << PIN_CLK) | (1 << PIN_DATA));
    memset(buf, 0, sizeof(buf));
    cur_plane   = 0;
    plane_ticks = PLANE_WEIGHT[0];
}

void display_scan() {
    uint8_t* front = buf[front_idx];

    // --- Brightness scaling via temporal dithering ---
    // Each plane has a full_weight (1/2/4/8 ticks per cycle).
    // We split those ticks into:
    //   data_ticks  = round(full_weight * brightness / 255)  → show actual plane
    //   blank_ticks = full_weight - data_ticks               → shift all-zero
    //
    // tick_num = how many ticks into this plane we already are (0-indexed).
    // plane_ticks counts DOWN from full_weight to 1, so:
    //   tick_num = full_weight - plane_ticks

    uint8_t full_weight = PLANE_WEIGHT[cur_plane];
    uint8_t tick_num    = full_weight - plane_ticks;   // 0 = first tick

    // Compute how many of this plane's ticks show real data
    // Use integer multiply then divide by 255 (approximated as >>8 after *256 adjustment)
    uint8_t data_ticks = (uint8_t)(((uint16_t)full_weight * brightness + 127) / 255);

    const uint8_t* src = (tick_num < data_ticks)
                         ? (front + cur_plane * PLANE_BYTES)
                         : blank;
    shift_plane(src);

    // Advance state
    plane_ticks--;
    if (plane_ticks == 0) {
        cur_plane   = (cur_plane + 1) % BITPLANES;
        plane_ticks = PLANE_WEIGHT[cur_plane];
    }

    // Swap buffers at the very start of a new cycle (plane 0, tick 0) to avoid tearing
    if (cur_plane == 0 && plane_ticks == PLANE_WEIGHT[0] && swap_pending) {
        front_idx   ^= 1;
        swap_pending = 0;
    }
}

uint8_t* display_back_buffer() {
    return buf[front_idx ^ 1];
}

void display_swap() {
    swap_pending = 1;
}

void display_clear_back() {
    memset(buf[front_idx ^ 1], 0, FRAME_BYTES);
}

void display_set_brightness(uint8_t b) {
    brightness = b;
}
