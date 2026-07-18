#pragma once
#include <stdint.h>

// Hardware SPI pins — DEFAULT mapping on the 14-pin ATtiny1614 (no PORTMUX)
//   PA1 — SPI0 MOSI  → Frekvens DATA
//   PA2 — SPI0 MISO  → not connected
//   PA3 — SPI0 SCK   → Frekvens CLK
//   PA7 — GPIO       → Frekvens LATCH (manual pulse after each scan)
//   PA5 — TCB0 WO    → Frekvens /OE (active-low Output Enable, hardware PWM)
//
// /OE drives the SCT2024 drivers' output enable. It MUST be low for the LEDs to
// light. We drive it with a ~78 kHz TCB0 PWM for global brightness that preserves
// the full per-pixel 8-bit grayscale (see display_set_brightness).
//
// SPI runs at F_CPU/2 = 10 MHz, LSB-first, Mode 0.
// Each 32-byte plane scan takes ~25.6 µs.
// With 8-bit PWM (255 weighted ticks), visual refresh ≈ 153 Hz.

void    display_init();

// Shift one bit-plane to the Frekvens panel.
// Call as fast as possible from the main loop — the function itself
// controls inter-plane weighting and brightness via internal state.
void    display_scan();

// Pointer to the back (off-screen) framebuffer.
// Write 256 bytes of pre-computed bit-plane data here, then call display_swap().
uint8_t* display_back_buffer();

// Schedule a front/back buffer swap. Swap executes at the start of the
// next complete PWM cycle to prevent tearing.
void    display_swap();

// Apply a pending swap immediately (called from the RX ISR before capturing a
// new frame, so it never writes into the buffer about to be displayed).
void    display_force_swap();

// Zero the back buffer.
void    display_clear_back();

// Global brightness scale (0 = off, 255 = full).
// Drives the /OE hardware PWM duty cycle (TCB0). Independent of the bit-plane
// data, so it dims the whole panel while preserving full 8-bit per-pixel grayscale.
void    display_set_brightness(uint8_t b);
