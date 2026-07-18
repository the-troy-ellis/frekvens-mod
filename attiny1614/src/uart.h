#pragma once
#include <stdint.h>

// Chain protocol engine — hardware USART0 @ 1 Mbaud, 8N1.
//   PB2 — USART0 TXD → 3.5mm Tip out (downstream to next panel)
//   PB3 — USART0 RXD → 3.5mm Tip in  (upstream from ESP32 / previous panel)
//
// SWALLOW-FIRST-FRAME daisy chaining
// ---------------------------------
// The ESP32 sends, per refresh:  [F0][F1]…[F(n-1)] [SHOW]
// where each Fk = FE ED CMD_FRAME <256 bytes> <crc>, and SHOW = FE ED CMD_SHOW.
//
// Every panel runs IDENTICAL firmware regardless of its position. Each panel:
//   • Captures the FIRST frame of each cycle into its display back buffer and
//     does NOT forward those bytes ("swallows" them).
//   • Forwards everything else verbatim to the next unit — so the downstream
//     panel sees the NEXT frame first, and so on down the chain.
//   • Applies SHOW (swap), CLEAR, and BRIGHTNESS globally and forwards them too.
// A cycle restarts at each SHOW, so positions are self-assigning — no per-chip ID.
//
// All of this runs inside the RX interrupt because the forward/echo decision is
// real-time: bytes already sent downstream can't be recalled. The main loop only
// drives the display.

void uart_init();

// True once the first valid command has been received from the ESP32.
// Used by the main loop to end the boot self-test.
bool chain_is_active();

// Service deferred work that is too slow to run inside the RX interrupt
// (currently: CMD_CLEAR, which memsets the back buffer). Call from the main loop.
void chain_service();
