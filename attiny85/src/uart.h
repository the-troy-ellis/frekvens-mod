#pragma once
#include <stdint.h>

#define UART_BAUD  115200
#define PIN_RX     PB3
#define PIN_TX     PB4

void    uart_init();

// Non-blocking: if start bit is present right now, receive and echo byte, set *ok=1.
// If line is idle, *ok=0, returns 0 immediately.
uint8_t uart_poll(uint8_t* ok);

// Blocking receive + echo. Caller should interleave display_scan() themselves
// for frames — this is used only for single-byte reads (CMD, CRC, brightness byte).
uint8_t uart_read_blocking();

void    uart_write(uint8_t b);
