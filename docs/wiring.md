# Wiring Guide

## ATtiny1614 per Frekvens Panel

The ATtiny1614 replaces the original ATtiny85 design. It runs at 20 MHz with
hardware UART and hardware SPI, giving significantly better performance.

### Pin Assignment

SOIC-14 physical pin numbers (pin 1 at the notch, counting down the left side
then up the right). All peripherals use their **default** pin mapping — no PORTMUX
remap, because the SPI alternate mapping targets a PORTC that the 14-pin package lacks.

| Pin | Port | Direction | Connected To |
|:---:|:---:|:---:|---|
| 11 | PA1 | OUT | Frekvens **DATA** (SPI0 MOSI, default) — *yellow* |
| 13 | PA3 | OUT | Frekvens **CLK**  (SPI0 SCK,  default) — *white* |
| 5  | PA7 | OUT | Frekvens **LATCH** (GPIO, pulsed after each scan) — *green* |
| 3  | PA5 | OUT | Frekvens **/OE** (TCB0 WO, ~78 kHz brightness PWM) — *blue* |
| 7  | PB2 | OUT | 3.5mm **Tip** — UART TX (USART0 TXD) to downstream unit — *green* |
| 6  | PB3 | IN  | 3.5mm **Tip** — UART RX (USART0 RXD) from upstream / ESP32 — *yellow* |
| 10 | PA0 | —   | **UPDI** — leave connected to programming header — *white* |
| 1  | VCC | 3.3V | Frekvens internal 3.3V rail — *red* |
| 14 | GND | GND  | Frekvens internal GND — *black* |
| 12 | PA2 | — | SPI0 MISO — leave unconnected (panel is write-only) |
| 2,4,8,9 | PA4,PA6,PB1,PB0 | — | free for future use |

**/OE (Output Enable)** — the SCT2024 LED drivers will not light *at all* unless
/OE is driven low. The firmware drives PA5 (TCB0 WO) and **must** be connected to
the panel's /OE line. A ~78 kHz PWM on this pin sets global brightness while the
bit-plane scan keeps full per-pixel grayscale.

**Fit a 10 kΩ pull-up from /OE to 3.3V.** /OE is active-low, so a floating line
means "outputs enabled". Between power-on and the firmware's first instruction
the ATtiny cannot drive anything — POR, the BOD hold, and the 64 ms startup
delay all leave PA5 high-impedance — and during that window the SCT2024 shift
registers still hold power-up garbage. Without the pull-up an arbitrary subset
of all 256 LEDs can switch on at full current for 60+ ms while the supply is
still ramping — the suspected mechanism **B** behind the cold-start failure
documented under *Known issue* below (not yet confirmed on hardware; the
measurement that settles it is described there). The pull-up holds the drivers
blanked until the firmware takes over; `display_init()`
covers the window from its first instruction by raising /OE before PA5 becomes
an output, latching a zero frame, and then holding the drivers blanked for a
further ~200 ms (`OE_SETTLE_MS`) so the rail can finish climbing to 3.3V before
any LED current is drawn at all.

**Do not repurpose PA0 as an external RESET for this.** It is tempting to wire
a front-panel button to a reset line and "restart once power is up", but PA0 is
a single shared pin — UPDI *or* RESET *or* GPIO — so taking RESET costs UPDI
programming and needs 12V HV-UPDI hardware to undo. It also does not help:
BOD already holds the chip until VCC is good, and while the part is in reset
PA5 floats, which is the very condition the pull-up exists to prevent. Holding
the MCU in reset for longer widens the uncontrolled window rather than closing
it.

The ATtiny1614 draws ~6 mA at 20 MHz — well within the Frekvens 3.3V rail headroom.

### Wire colours

> **Printable bench card:** [`docs/harness-card.html`](harness-card.html) renders
> this section as a 2-page A4 reference (pinout, bundles, chain orientation,
> power detail). Open it in a browser and print. Every colour is named in text
> as well as shown, so it stays usable on a mono printer.

Six colours are stocked: **black, white, blue, green (teal), red, yellow** — for
nine nets. Colours are therefore reused, under one rule:

> **Never reuse a colour within a bundle.** Red and black are reserved for power
> everywhere and are never a signal.

The reuse is safe because each bundle terminates on a physically different
connector — the panel bus on the Frekvens JST header, the chain on 3.5 mm panel
jacks, UPDI on its own 3-pin header — so a mis-plug *between* bundles is
mechanically impossible, not merely unlikely.

**Reserved — power, everywhere**

| Net | Colour |
|---|---|
| 3.3V | **red** |
| GND | **black** |

**Panel bus** — 4 signals to the Frekvens header; all four must differ

| Signal | Pin | Colour | Mnemonic |
|---|:---:|---|---|
| DATA | 11 (PA1) | **yellow** | the payload |
| CLK | 13 (PA3) | **white** | clock face |
| LATCH | 5 (PA7) | **green** | go / commit |
| /OE | 3 (PA5) | **blue** | blanking — the "off" control |

**Chain** — 3.5 mm jack tips (both sleeves are black/GND)

| Signal | Pin | Colour |
|---|:---:|---|
| UART TX → **OUT** jack | 7 (PB2) | **green** |
| UART RX ← **IN** jack | 6 (PB3) | **yellow** |

Swapping TX and RX is the classic daisy-chain error, and the two jacks are
visually identical once the case is closed — so the wire colour is the only
thing telling them apart. Green and yellow are the most separable pair of the
four signal colours under warm workshop light. **Green always lands on OUT.**

**UPDI header** — 3 pins: UPDI, 3.3V, GND

| Signal | Pin | Colour |
|---|:---:|---|
| UPDI | 10 (PA0) | **white** |

White is unambiguous inside that header (its only companions are red and
black). Worth a band of heatshrink or a marker stripe anyway: PA0 is the one
net where a wiring mistake costs you the ability to reprogram the chip.

**The two cold-start additions** (see *Known issue* below) carry power colours,
not signal colours: the 10 kΩ /OE pull-up bridges the **blue** /OE net to
**red** 3.3V, and the 100 µF bulk cap sits **red** (+) to **black** (−) right at
the ATtiny's VCC/GND pins.

### Frekvens Panel Connector

The Frekvens panel exposes **DATA**, **CLK**, **LATCH**, **3.3V**, and **GND** on a
small JST-style header after the original PCB is removed. Refer to
`frekvens-hacking.pdf` for the exact pinout on your panel revision.

The existing inter-unit power connectors and built-in power supply in each unit
are left completely intact. They continue to power the LED panel and now also
supply the ATtiny1614.

---

## 3.5mm TS (Mono) Daisy-Chain

Each unit has two **3.5mm TS panel-mount jacks** — one IN, one OUT.
The second button hole on the back of the unit is used as the mounting hole
for one of these jacks.

| 3.5mm Contact | Signal |
|:---:|---|
| **Tip**    | UART data |
| **Sleeve** | GND (signal reference only) |

A standard **mono 3.5mm patch cable** connects adjacent units tip-to-tip.
Power is not carried by the data cable — each unit is self-powered.

```
ESP32 TX ──────────────► Panel 0 IN jack   (PB3, pin 6 — ATtiny RX)
                         Panel 0 OUT jack  (PB2, pin 7 — ATtiny TX)
                              └──TS──────► Panel 1 IN jack   (PB3, pin 6)
                                           Panel 1 OUT jack  (PB2, pin 7)
                                                └──TS──────► ...
```

### Jack sourcing

**PJ-138A / PJ-3502** style panel-mount mono 3.5mm jacks — nut-mount, 6mm hole.
The original Frekvens button hole may need opening slightly with a step drill.
Eurorack patch cables are ideal (TS, short lengths, widely available).

---

## ESP32 Connections

| ESP32 Pin | Connected To |
|:---:|---|
| UART TX | Panel 0 **IN** jack Tip — PB3, pin 6 (ATtiny RX) |
| UART RX | Panel 0 **OUT** jack Tip — PB2, pin 7 (ATtiny TX) — optional, diagnostics |
| GND | Panel 0 GND (signal reference only) |

> **Which GPIOs?** This depends on the ESP32 board, and the repo currently
> disagrees with itself. The firmware targets a **Seeed XIAO ESP32-S3**
> (`esp32/platformio.ini`) and uses **GPIO1 = TX, GPIO2 = RX**
> (`esp32/src/config.h`) — deliberately off the USB-console UART. The BOM below
> still lists an **ESP32-WROOM-32**, whose UART2 would be GPIO17/GPIO16. Confirm
> which module is actually in the build and correct the other reference; the
> ATtiny side (PB3 in / PB2 out) is the same either way.

The ESP32 is powered independently. Its GND shares a reference with Panel 0
for UART signal levels — a single wire is sufficient.

---

## ATtiny1614 Programming

Programs via **UPDI** — a single-wire interface on PA0 (pin 10).

**No fuse burning required** — the default 20 MHz oscillator just needs selecting.
UPDI stays enabled so the chip remains reprogrammable at any time.

**Programmer options:**

| Option | Hardware needed | PlatformIO protocol |
|---|---|---|
| SerialUPDI | USB-serial adapter + 470 Ω resistor on TX→UPDI | `serialupdi` |
| jtag2updi  | Arduino Nano flashed as UPDI bridge | `jtag2updi` |
| Atmel-ICE  | Atmel-ICE debugger | `atmelice_updi` |

SerialUPDI is the cheapest: any CH340 or CP2102 USB-serial adapter works.

```bash
cd attiny1614
pio run --target fuses    # set 20 MHz oscillator fuse
pio run --target upload   # flash firmware
```

Update the `upload_port` in `platformio.ini` to match your serial adapter's device path.

---

## Power

Each Frekvens unit contains its own power supply (4V DC, 1.5A) and the units
are chained together via their original power connectors. This infrastructure
is kept entirely intact.

The ATtiny1614 and the ESP32 need 3.3V. The Frekvens 4V supply feeds a small
low-dropout regulator that provides 3.3V for the panel logic. Tap this 3.3V
rail for the ATtiny1614 (it draws ~6 mA). For the ESP32, use a dedicated LDO
(AP2112K-3.3 or equivalent, 600 mA rated) from the 4V rail — this keeps the
ESP32's WiFi peak current off the panel's supply.

```
Frekvens 4V rail ──┬── existing panel supply (shift registers, LEDs)
                   ├── ATtiny1614 VCC (tap existing 3.3V rail)
                   └── AP2112K-3.3 ──── ESP32 3.3V pin
```

| Load | Current |
|---|---|
| Frekvens LED panel (all on) | ~120 mA |
| ATtiny1614 @ 20 MHz | ~6 mA |
| ESP32 average (WiFi active) | ~100 mA |
| ESP32 peak (WiFi TX burst) | ~240–400 mA |
| AP2112K-3.3 handles (ESP32 only) | 600 mA rated |

### Known issue: intermittent cold-start

Symptom: on a fully-cold plug-in (unplugged 30+ s) the panel sometimes fails to
light. A quick unplug/re-plug starts it reliably; another 30+ s wait can fail
again.

This is **not** a fuse problem. A read-back of a field chip (2026-07-28) confirmed
BOD is already enabled (2.6 V, continuous), so the chip correctly holds in reset
until VCC clears 2.6 V. Two things are true and both point at the 3.3 V rail:
the ATtiny taps the **panel-logic LDO shared with the LED shift registers**, and
the BOM carried only a 100 nF decoupling cap — **no bulk capacitance at all** on
that rail.

There are two candidate mechanisms, and they are not mutually exclusive. The
shared, bulk-less rail is what makes either one bite:

**A — passive slow ramp.** On a fully-discharged cold start the rail ramps
softly and dwells/sags near 2.6 V, so BOD simply never releases and the chip
never starts. A quick re-plug pre-charges the caps and the rail snaps up cleanly.

**B — LED inrush knocking the rail back down.** BOD *does* release at 2.6 V, the
firmware starts, and (before the fix below) `display_init()` drove /OE low while
the SCT2024 shift registers still held power-up garbage — switching an arbitrary
subset of all 256 LEDs on at full current into a rail that is still climbing.
That collapses it back under 2.6 V, BOD resets, /OE floats, the rail recovers,
and it loops. A quick re-plug rides through on the charged bulk caps.

**The same measurement tells them apart.** Meter (better: scope) the 3.3 V rail
during a *failed* cold start:
- sitting flat at ~2.x V and never rising → **A**
- climbing then repeatedly collapsing → **B**

Fixes, in the order worth trying:

1. **Reflash the ATtiny** (free, no soldering). `display_init()` now raises /OE
   before PA5 becomes an output, latches a zero frame into the drivers, and holds
   them blanked for `OE_SETTLE_MS` (~200 ms) before connecting the PWM — so no LED
   current is drawn while the rail is still climbing. Closes **B** from the
   firmware's first instruction onward.
2. **10 kΩ pull-up from /OE to 3.3 V** (see the /OE note above). Closes the ~64 ms
   window *before* the firmware runs, which it cannot reach itself.
3. **~100 µF bulk cap on the ATtiny 3.3 V rail, near VCC.** Addresses **A**
   directly, and gives the rail energy to ride through any residual inrush.

Useful isolation step either way: power the ATtiny alone (ESP32 disconnected) and
cold-start it ~10× to rule the ESP32's WiFi inrush in or out of the shared rail.

---

## Bill of Materials (per panel unit)

| Component | Qty | Notes |
|---|:---:|---|
| IKEA Frekvens LED panel | 1 | Any revision — original power chain kept |
| ATtiny1614-SSF (SOIC-14) | 1 | 20 MHz rated, 3.3V operation |
| 3.5mm TS panel-mount jack (mono) | 2 | PJ-138A/PJ-3502, 6mm hole, nut-mount |
| 100 nF decoupling capacitor | 1 | ATtiny1614 VCC bypass |
| 10 kΩ resistor | 1 | **/OE pull-up to 3.3V — required for a reliable cold start** |
| 100 µF electrolytic/tantalum | 1 | **bulk on the ATtiny 3.3V rail — the rail is shared with the LED drivers and had none** |
| UPDI 3-pin header (PA0, VCC, GND) | 1 | Leave accessible for reprogramming |
| SOIC-14 adapter / custom PCB | 1 | ATtiny1614 is SMD only |

**ESP32 coordinator (one per installation):**

| Component | Qty | Notes |
|---|:---:|---|
| ESP32-WROOM-32 module | 1 | Bare module, 18×25mm |
| AP2112K-3.3 LDO (SOT-23-5) | 1 | 4V → 3.3V for ESP32, 600 mA rated |
| 100 nF + 10 µF caps | 2 | LDO input/output bypass |

**Cables:** standard mono 3.5mm patch cables (TS). Eurorack patch cables are
the correct connector type and come in short lengths.
