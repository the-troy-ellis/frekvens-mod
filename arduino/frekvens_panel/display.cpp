#include "display.h"
#include "protocol.h"
#include <avr/io.h>
#include <avr/interrupt.h>
#include <string.h>

// --- Framebuffers ---
// Two complete 8-plane framebuffers: 2 × 256 bytes = 512 bytes
static uint8_t buf[2][FRAME_BYTES];
static volatile uint8_t  front_idx    = 0;
static volatile uint8_t  swap_pending = 0;

// Scan state. Bit-angle modulation: instead of showing each plane for a
// contiguous 2^p ticks (which makes the MSB a long on/off block that flickers at
// the ~74 Hz cycle rate), we pick the plane per tick as 7 - (trailing 1-bits of
// the tick counter). That shows plane 7 on every even tick, plane 6 on every 4th,
// etc. — each plane still appears 2^p times per 255-tick cycle (identical
// grayscale), but distributed, so the MSB toggles ~9 kHz and mid-greys don't
// visibly flicker.
static uint16_t tick = 0;          // 0..254 within a cycle
static const uint8_t* front_buf = buf[0];

// --- Pins ---
//   MOSI=PA1 (DATA), SCK=PA3 (CLK), LATCH=PA7, /OE=PA5 (TCB0 WO, hardware PWM)
#define LATCH_HIGH()  PORTA.OUTSET = PIN7_bm
#define LATCH_LOW()   PORTA.OUTCLR = PIN7_bm

// --- SPI transfer of one 32-byte plane ---
static void spi_send_plane(const uint8_t* src) {
    SPI0.DATA = src[0];
    for (uint8_t i = 1; i < PLANE_BYTES; i++) {
        while (!(SPI0.INTFLAGS & SPI_IF_bm));   // wait for byte i-1 done
        SPI0.DATA = src[i];                      // start byte i (also clears IF)
    }
    while (!(SPI0.INTFLAGS & SPI_IF_bm));        // wait for last byte
    LATCH_HIGH();
    __asm__ volatile("nop\nnop");
    LATCH_LOW();
}

// --- Public API ---

void display_init() {
    // SPI0 default pins on the 14-pin part: MOSI=PA1, MISO=PA2, SCK=PA3, SS=PA4.
    // LATCH = PA7 (GPIO). /OE = PA5 (driven by TCB0 PWM, see below).
    //
    // ORDER IS LOAD-BEARING: /OE must be driven HIGH (outputs disabled) BEFORE
    // PA5 becomes an output. PORTx.OUT resets to 0, so setting DIR first would
    // drive /OE low and enable the SCT2024 outputs while their shift registers
    // still hold power-up garbage — lighting an arbitrary subset of all 256
    // LEDs at full current for the ~1-3 ms it takes to reach the first latch
    // (this init, plus uart_init()'s CRC table build). On a cold start that
    // inrush drags the still-ramping 3.3V rail back under the BOD threshold,
    // the chip resets, /OE floats, the rail recovers, and it loops forever —
    // which is exactly the "won't start cold, starts fine on a quick replug"
    // failure (a warm replug rides through on the supply's charged bulk caps).
    PORTA.OUTSET  = PIN5_bm;        // /OE high = outputs DISABLED, before DIR
    PORTA.DIR    |= PIN1_bm | PIN3_bm | PIN5_bm | PIN7_bm;
    PORTA.OUTCLR  = PIN7_bm;        // LATCH idle low

    // SPI: master, LSB-first, Mode 0. CLK2X + DIV4 = F_CPU/2 = 5 MHz at 10 MHz.
    SPI0.CTRLA = SPI_DORD_bm | SPI_ENABLE_bm | SPI_MASTER_bm
               | SPI_CLK2X_bm | SPI_PRESC_DIV4_gc;
    SPI0.CTRLB = SPI_SSD_bm | SPI_MODE_0_gc;

    // Clock a known-zero frame into the drivers and latch it while /OE is still
    // disabled, so the outputs are guaranteed dark before they are ever enabled.
    // One plane is a full 256-output register load.
    memset(buf, 0, sizeof(buf));
    front_buf = buf[0];
    tick      = 0;
    spi_send_plane(buf[0]);

    // --- Global brightness via hardware PWM on /OE (PA5 = TCB0 WO) ---
    // TCB0 8-bit PWM: period = CCMPL+1 = 256 ticks at CLK_PER (10 MHz) = ~39 kHz.
    // The output is HIGH for CCMPH ticks each period. /OE is active-low, so:
    //   LED-on fraction = (256 - CCMPH) / 256.
    //   CCMPH = 0   → /OE always low  → full brightness.
    //   larger CCMPH → /OE high more  → dimmer.
    // ~39 kHz is far above the bit-plane refresh (~77 Hz), so it dims uniformly
    // across all planes and preserves the full 8-bit per-pixel grayscale.
    TCB0.CCMPL   = 255;                                 // period
    TCB0.CCMPH   = 0;                                    // start at full brightness
    TCB0.CTRLB   = TCB_CNTMODE_PWM8_gc | TCB_CCMPEN_bm;  // 8-bit PWM, drive WO pin
    TCB0.INTCTRL = 0;                                    // no interrupt (in case the
                                                        //   Arduino core used TCB0 for millis)
    TCB0.CTRLA   = TCB_ENABLE_bm;                        // CLKSEL=0 → CLK_PER, enable
}

void display_scan() {
    // Bit-angle modulation: plane = (BITPLANES-1) - (# trailing 1-bits of tick).
    // Over one 255-tick cycle plane p is shown exactly 2^p times (identical
    // grayscale to weighted bit-planes) but spread out, so the MSB toggles fast
    // and mid-greys don't flicker. Global brightness is the hardware /OE PWM.
    uint8_t t = (uint8_t)tick;
    uint8_t p = BITPLANES - 1;
    while (t & 1) { p--; t >>= 1; }        // count trailing ones (max 7)
    spi_send_plane(front_buf + (uint16_t)p * PLANE_BYTES);

    // 255 ticks (0..254) per full cycle; swap at the cycle boundary.
    if (++tick >= ((1u << BITPLANES) - 1)) {
        tick = 0;
        uint8_t s = SREG;
        cli();
        if (swap_pending) {
            front_idx   ^= 1;
            front_buf    = buf[front_idx];
            swap_pending = 0;
        }
        SREG = s;
    }
}

uint8_t* display_back_buffer() {
    return buf[front_idx ^ 1];
}

void display_swap() {
    swap_pending = 1;
}

void display_force_swap() {
    // Apply a still-pending swap immediately. Called from the RX ISR before it
    // latches a capture buffer: with only two buffers, if the previous SHOW's
    // swap hasn't executed yet (frames arriving faster than the PWM cycle, i.e.
    // >~75fps), capturing into buf[front_idx^1] would clobber the frame that is
    // about to be shown. Resolving the swap here frees a clean buffer. At the
    // ESP32's 20fps the swap is always already done, so this is a no-op.
    if (swap_pending) {
        front_idx   ^= 1;
        front_buf    = buf[front_idx];
        swap_pending = 0;
    }
}

void display_clear_back() {
    memset(buf[front_idx ^ 1], 0, FRAME_BYTES);
}

void display_set_brightness(uint8_t b) {
    // Global brightness via the /OE PWM duty cycle (TCB0).
    if (b == 0) {
        // True off: disconnect the timer from the pin and hold /OE high (disabled).
        TCB0.CTRLB &= ~TCB_CCMPEN_bm;
        PORTA.OUTSET = PIN5_bm;
        return;
    }
    // on_counts = round(256 * b / 255), 1..256. CCMPH = 256 - on_counts (0..255).
    uint16_t on_counts = ((uint16_t)256 * b + 127) / 255;
    if (on_counts > 256) on_counts = 256;
    TCB0.CCMPH  = (uint8_t)(256 - on_counts);
    TCB0.CTRLB |= TCB_CCMPEN_bm;     // (re)connect the timer output to /OE
}
