#include <avr/io.h>
#include <avr/interrupt.h>
#include "display.h"
#include "uart.h"
#include "self_test.h"

// The chain protocol engine in uart.cpp runs entirely in the RX interrupt:
// it captures this panel's frame into the display back buffer, forwards all
// other bytes downstream, and applies SHOW/CLEAR/BRIGHTNESS. The main loop is
// therefore just the display driver plus the boot self-test.

// Clock is configured by the megaTinyCore board "Clock" setting (use 10 MHz
// internal — in-spec at 3.3V; 20 MHz needs 4.5V on the 1-series). The UART baud
// divisor is derived from F_CPU, so the firmware works at whatever clock is set.

void setup() {
    display_init();
    uart_init();
    sei();

    // Watchdog (~1 s). The main loop pets it every iteration. If the firmware
    // ever stalls — e.g. the SPI transfer loop hangs after a power glitch that
    // didn't dip below the BOD threshold — the WDT resets the chip and it
    // restarts cleanly instead of sitting dark. Belt-and-suspenders with BOD.
    _PROTECTED_WRITE(WDT.CTRLA, WDT_PERIOD_1KCLK_gc);

    // Boot self-test runs until the ESP32 sends its first command.
    selftest_start();
}

void loop() {
    static bool test_ended = false;

    __asm__ __volatile__("wdr");   // pet the watchdog

    // Drive the display PWM. Called continuously; the ISR captures frames
    // concurrently, so reception never pauses the display.
    display_scan();

    if (!chain_is_active()) {
        // No data yet — keep cycling the self-test.
        selftest_tick();
    } else if (!test_ended) {
        // First command arrived — leave the self-test at full brightness.
        selftest_end();
        test_ended = true;
    }

    // Handle deferred protocol work (e.g. CMD_CLEAR).
    chain_service();
}
