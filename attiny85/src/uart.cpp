#include "uart.h"
#include <avr/io.h>
#include <util/delay.h>

// At 16MHz: 16,000,000 / 115200 = 138.9 cycles per bit
#define BIT_CYCLES       139
#define HALF_BIT_CYCLES  70    // centre of start bit for initial sample

// _delay_loop_2: each iteration = 4 cycles
#define DELAY_CYCLES(n)  _delay_loop_2((n) / 4)

void uart_init() {
    DDRB  &= ~(1 << PIN_RX);
    PORTB |=  (1 << PIN_RX);   // pull-up
    DDRB  |=  (1 << PIN_TX);
    PORTB |=  (1 << PIN_TX);   // idle high
}

// Read 8 data bits. Called immediately after start bit is detected;
// HALF_BIT_CYCLES must already have elapsed before the first data sample.
static uint8_t recv_bits() {
    uint8_t data = 0;
    for (uint8_t i = 0; i < 8; i++) {
        DELAY_CYCLES(BIT_CYCLES);
        data >>= 1;
        if (PINB & (1 << PIN_RX)) data |= 0x80;
    }
    DELAY_CYCLES(BIT_CYCLES);  // consume stop bit time
    return data;
}

static void uart_write_raw(uint8_t b) {
    PORTB &= ~(1 << PIN_TX);            // start bit
    DELAY_CYCLES(BIT_CYCLES);
    for (uint8_t i = 0; i < 8; i++) {
        if (b & 0x01) PORTB |=  (1 << PIN_TX);
        else          PORTB &= ~(1 << PIN_TX);
        b >>= 1;
        DELAY_CYCLES(BIT_CYCLES);
    }
    PORTB |= (1 << PIN_TX);             // stop bit
    DELAY_CYCLES(BIT_CYCLES);
}

uint8_t uart_poll(uint8_t* ok) {
    // Check for start bit RIGHT NOW (no wait)
    if (PINB & (1 << PIN_RX)) { *ok = 0; return 0; }
    *ok = 1;
    DELAY_CYCLES(HALF_BIT_CYCLES);      // skip to centre of start bit
    uint8_t b = recv_bits();
    uart_write_raw(b);                  // echo downstream
    return b;
}

uint8_t uart_read_blocking() {
    while (PINB & (1 << PIN_RX));       // wait for start bit
    DELAY_CYCLES(HALF_BIT_CYCLES);
    uint8_t b = recv_bits();
    uart_write_raw(b);
    return b;
}

void uart_write(uint8_t b) {
    uart_write_raw(b);
}
