#include "uart.h"
#include "display.h"
#include "protocol.h"
#include <avr/io.h>
#include <avr/interrupt.h>

// Per-byte CRC-8 update (poly 0x31, matches the ESP32 renderer's crc8()).
// Bit-by-bit — used only at startup to build the lookup tables below.
static uint8_t crc8_bits(uint8_t crc, uint8_t b) {
    for (uint8_t i = 0; i < 8; i++) {
        if ((crc ^ b) & 0x80) crc = (crc << 1) ^ 0x31;
        else                   crc = (crc << 1);
        b <<= 1;
    }
    return crc;
}

// Table-driven CRC so the RX interrupt stays under the 1-byte time budget at
// 10 MHz / 1 Mbaud (the bit-loop was ~12µs/byte > the 10µs byte period, causing
// dropped bytes on 256-byte frames). This CRC is linear over GF(2), so
//   crc8_bits(crc, b) == crcF[crc] ^ crcG[b]
// (verified: two 256-byte tables reproduce the bit-loop exactly). The ISR then
// does two lookups + an XOR (~a few cycles) instead of an 8-iteration loop.
static uint8_t crcF[256];
static uint8_t crcG[256];

static void crc_tables_init() {
    for (uint16_t i = 0; i < 256; i++) {
        crcF[i] = crc8_bits((uint8_t)i, 0);
        crcG[i] = crc8_bits(0, (uint8_t)i);
    }
}
static inline uint8_t crc8_upd(uint8_t crc, uint8_t b) {
    return crcF[crc] ^ crcG[b];
}

// Send one byte, waiting for the data register to be free first.
static inline void txw(uint8_t b) {
    while (!(USART0.STATUS & USART_DREIF_bm));
    USART0.TXDATAL = b;
}

// --- Protocol phases ---
enum {
    P_CAP_HDR = 0,   // cycle start: matching my frame header (echo held back)
    P_CAP_DATA,      // storing my 256 frame bytes (swallowed, not forwarded)
    P_CAP_CRC,       // reading my frame's CRC byte (swallowed)
    P_CAP_BRI,       // reading a brightness value that appeared at cycle start
    P_FORWARD        // my frame done: forward all bytes until the next SHOW
};

static volatile bool chain_active  = false;
static volatile bool clear_request = false;

// Capture state
static uint8_t  phase       = P_CAP_HDR;
static uint8_t  cap_s       = 0;        // header match (0=idle,1=got FE,2=got ED)
static uint8_t* cap_ptr     = 0;        // latched back-buffer pointer for this frame
static uint16_t cap_idx     = 0;
static uint8_t  cap_crc     = 0;
static bool     frame_valid = false;    // my captured frame passed CRC, awaiting SHOW

// Forward state
static uint8_t  fwd_s    = 0;           // magic match while forwarding (0,1,2)
static uint16_t fwd_skip = 0;           // bytes to pass through without scanning
static bool     fwd_bri  = false;       // next forwarded byte is a brightness value

static inline void do_show() {
    // Display the frame we captured this cycle (if any passed CRC).
    if (frame_valid) { display_swap(); frame_valid = false; }
}

static inline void enter_capture() {
    phase = P_CAP_HDR;
    cap_s = 0;
}

// RX complete: the whole chain protocol runs here.
ISR(USART0_RXC_vect) {
    uint8_t b = USART0.RXDATAL;

    switch (phase) {

    // ---- Cycle start: decide if the first frame is mine, holding the header ----
    case P_CAP_HDR:
        if (cap_s == 0) {
            if (b == PROTO_MAGIC_0) cap_s = 1;          // hold a potential FE
            else                    txw(b);             // stray byte → forward
        } else if (cap_s == 1) {
            if      (b == PROTO_MAGIC_1) cap_s = 2;      // hold FE ED
            else if (b == PROTO_MAGIC_0) txw(PROTO_MAGIC_0);   // FE FE: flush one, hold one
            else { txw(PROTO_MAGIC_0); txw(b); cap_s = 0; }    // not magic: flush both
        } else {                                         // cap_s == 2: command byte
            cap_s = 0;
            chain_active = true;
            switch (b) {
            case CMD_FRAME:                              // MINE — swallow header + data
                display_force_swap();                    // free a clean buffer first
                cap_ptr = display_back_buffer();
                cap_idx = 0;
                cap_crc = 0;
                phase   = P_CAP_DATA;
                break;
            case CMD_SHOW:                               // forward held header + act
                txw(PROTO_MAGIC_0); txw(PROTO_MAGIC_1); txw(CMD_SHOW);
                do_show();
                break;
            case CMD_CLEAR:
                txw(PROTO_MAGIC_0); txw(PROTO_MAGIC_1); txw(CMD_CLEAR);
                clear_request = true;
                break;
            case CMD_BRIGHTNESS:
                txw(PROTO_MAGIC_0); txw(PROTO_MAGIC_1); txw(CMD_BRIGHTNESS);
                phase = P_CAP_BRI;
                break;
            default:                                     // unknown: forward as-is
                txw(PROTO_MAGIC_0); txw(PROTO_MAGIC_1); txw(b);
                break;
            }
        }
        break;

    // ---- Storing my frame's 256 data bytes (not forwarded) ----
    case P_CAP_DATA:
        cap_ptr[cap_idx] = b;
        cap_crc = crc8_upd(cap_crc, b);
        if (++cap_idx == FRAME_BYTES) phase = P_CAP_CRC;
        break;

    // ---- My frame's CRC byte (not forwarded) ----
    case P_CAP_CRC:
        frame_valid = (b == cap_crc);
        phase    = P_FORWARD;
        fwd_s    = 0;
        fwd_skip = 0;
        fwd_bri  = false;
        break;

    // ---- Brightness value that appeared at cycle start ----
    case P_CAP_BRI:
        txw(b);
        display_set_brightness(b);
        enter_capture();
        break;

    // ---- My frame captured: forward everything until SHOW ----
    case P_FORWARD:
        txw(b);                                          // forward 1:1
        if (fwd_bri)  { display_set_brightness(b); fwd_bri = false; break; }
        if (fwd_skip) { fwd_skip--; break; }             // inside a frame body — don't scan
        if (fwd_s == 0) {
            if (b == PROTO_MAGIC_0) fwd_s = 1;
        } else if (fwd_s == 1) {
            if      (b == PROTO_MAGIC_1) fwd_s = 2;
            else if (b == PROTO_MAGIC_0) fwd_s = 1;
            else                          fwd_s = 0;
        } else {                                         // fwd_s == 2: command
            fwd_s = 0;
            switch (b) {
            case CMD_FRAME:      fwd_skip = FRAME_BYTES + 1; break;  // pass body+crc unscanned
            case CMD_SHOW:       do_show(); enter_capture();        break;
            case CMD_CLEAR:      clear_request = true;              break;
            case CMD_BRIGHTNESS: fwd_bri = true;                    break;
            default: break;
            }
        }
        break;
    }
}

#define CHAIN_BAUD  250000UL    // 250 kbaud — 1 Mbaud overran the RX ISR at 10 MHz on
                                // 256-byte frames. 40µs/byte gives the ISR ~5x headroom.

void uart_init() {
    crc_tables_init();   // build CRC lookup tables before any frame can arrive

    // USART0 DEFAULT pins: TXD = PB2, RXD = PB3 (no PORTMUX).
    PORTB.DIR |=  PIN2_bm;
    PORTB.DIR &= ~PIN3_bm;
    PORTB.PIN3CTRL = PORT_PULLUPEN_bm;   // hold RX idle-high when nothing drives it
                                         // (a floating RX picks up noise → false bytes)

    // Baud divisor derived from F_CPU so it stays correct at any clock
    // (10 MHz → 40, 20 MHz → 80). BAUD = 64*F_CPU / (16*baud).
    USART0.BAUD  = (uint16_t)((F_CPU * 64UL) / (16UL * CHAIN_BAUD));
    USART0.CTRLC = USART_CHSIZE_8BIT_gc;     // 8N1, async
    USART0.CTRLB = USART_TXEN_bm | USART_RXEN_bm;
    USART0.CTRLA = USART_RXCIE_bm;           // RX complete interrupt
}

bool chain_is_active() { return chain_active; }

void chain_service() {
    // CMD_CLEAR memsets the whole back buffer — too slow for the ISR, so it
    // sets a flag and we do the work here in the main loop.
    if (clear_request) {
        clear_request = false;
        display_clear_back();
        display_swap();
    }
}
