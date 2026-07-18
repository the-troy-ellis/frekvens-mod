#include <avr/io.h>
#include <avr/interrupt.h>
#include <string.h>
#include "protocol.h"
#include "display.h"
#include "uart.h"
#include "test_patterns.h"

// CRC-8 (Dallas/Maxim polynomial 0x31) — must match ESP32
static uint8_t crc8_buf(const uint8_t* data, uint8_t len) {
    uint8_t crc = 0x00;
    while (len--) {
        uint8_t byte = *data++;
        for (uint8_t i = 0; i < 8; i++) {
            if ((crc ^ byte) & 0x80) crc = (crc << 1) ^ 0x31;
            else                      crc = (crc << 1);
            byte <<= 1;
        }
    }
    return crc;
}

// Blocking byte read that keeps calling display_scan() while waiting.
// At 115200 baud a byte takes ~87µs; a scan takes ~96µs.
// This prevents display flicker during frame reception.
static uint8_t read_byte_scan() {
    uint8_t ok;
    for (;;) {
        display_scan();
        uint8_t b = uart_poll(&ok);
        if (ok) return b;
    }
}

static uint8_t handle_packet(uint8_t cmd) {
    switch (cmd) {

        case CMD_FRAME: {
            uint8_t* back = display_back_buffer();
            for (uint16_t i = 0; i < FRAME_BYTES; i++) {
                back[i] = read_byte_scan();
            }
            // Verify CRC over the received frame
            uint8_t rx_crc  = read_byte_scan();
            uint8_t exp_crc = crc8_buf(back, FRAME_BYTES);
            return (rx_crc == exp_crc) ? 1 : 0;
        }

        case CMD_SHOW:
            display_swap();
            return 1;

        case CMD_BRIGHTNESS:
            display_set_brightness(uart_read_blocking());
            return 1;

        case CMD_CLEAR:
            display_clear_back();
            display_swap();
            return 1;

        default:
            return 0;
    }
}

int main() {
    // Switch to 16MHz via internal PLL (clear prescaler)
    CLKPR = (1 << CLKPCE);
    CLKPR = 0x00;

    display_init();
    uart_init();
    sei();

    // --- Startup test ---
    // Run visual self-test for ~3 seconds so wiring can be verified
    // before the ESP32 connects.
    test_pattern_start();
    uint16_t boot_timeout = 30000;  // ~3s at ~10kHz scan rate
    uint8_t boot_ok;
    while (boot_timeout--) {
        test_pattern_tick();
        display_scan();
        uart_poll(&boot_ok);  // discard — just draining any noise
    }
    display_clear_back();
    display_swap();

    // --- Main loop ---
    uint8_t magic_state = 0;

    for (;;) {
        display_scan();

        uint8_t ok;
        uint8_t b = uart_poll(&ok);
        if (!ok) continue;

        // Sync magic detection (0xFE 0xED)
        if (magic_state == 0) {
            magic_state = (b == PROTO_MAGIC_0) ? 1 : 0;
            continue;
        }
        if (magic_state == 1) {
            if (b == PROTO_MAGIC_1)    { magic_state = 2; continue; }
            else if (b == PROTO_MAGIC_0) { magic_state = 1; continue; }
            else                         { magic_state = 0; continue; }
        }

        // magic_state == 2: b is CMD
        magic_state = 0;
        handle_packet(b);
    }
}
