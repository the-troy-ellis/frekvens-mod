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
| 11 | PA1 | OUT | Frekvens **DATA** (SPI0 MOSI, default) |
| 13 | PA3 | OUT | Frekvens **CLK**  (SPI0 SCK,  default) |
| 5  | PA7 | OUT | Frekvens **LATCH** (GPIO, pulsed after each scan) |
| 3  | PA5 | OUT | Frekvens **/OE** (TCB0 WO, ~78 kHz brightness PWM) |
| 7  | PB2 | OUT | 3.5mm **Tip** — UART TX (USART0 TXD) to downstream unit |
| 6  | PB3 | IN  | 3.5mm **Tip** — UART RX (USART0 RXD) from upstream / ESP32 |
| 10 | PA0 | —   | **UPDI** — leave connected to programming header |
| 1  | VCC | 3.3V | Frekvens internal 3.3V rail |
| 14 | GND | GND  | Frekvens internal GND |
| 12 | PA2 | — | SPI0 MISO — leave unconnected (panel is write-only) |
| 2,4,8,9 | PA4,PA6,PB1,PB0 | — | free for future use |

**/OE (Output Enable)** — the SCT2024 LED drivers will not light *at all* unless
/OE is driven low. The firmware drives PA5 (TCB0 WO) and **must** be connected to
the panel's /OE line. A ~78 kHz PWM on this pin sets global brightness while the
bit-plane scan keeps full per-pixel grayscale.

The ATtiny1614 draws ~6 mA at 20 MHz — well within the Frekvens 3.3V rail headroom.

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
ESP32 GPIO17 ─────────────── Panel 0 IN jack (PA2)
                              Panel 0 OUT jack (PA1) ──TS──► Panel 1 IN jack (PA2)
                                                              Panel 1 OUT jack (PA1) ──TS──► ...
```

### Jack sourcing

**PJ-138A / PJ-3502** style panel-mount mono 3.5mm jacks — nut-mount, 6mm hole.
The original Frekvens button hole may need opening slightly with a step drill.
Eurorack patch cables are ideal (TS, short lengths, widely available).

---

## ESP32 Connections

| ESP32 Pin | Connected To |
|:---:|---|
| GPIO17 (UART2 TX) | Panel 0 IN jack Tip (PA2 RX) |
| GPIO16 (UART2 RX) | Panel 0 OUT jack Tip (PA1 TX) — optional, diagnostics |
| GND | Panel 0 GND (signal reference only) |

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

---

## Bill of Materials (per panel unit)

| Component | Qty | Notes |
|---|:---:|---|
| IKEA Frekvens LED panel | 1 | Any revision — original power chain kept |
| ATtiny1614-SSF (SOIC-14) | 1 | 20 MHz rated, 3.3V operation |
| 3.5mm TS panel-mount jack (mono) | 2 | PJ-138A/PJ-3502, 6mm hole, nut-mount |
| 100 nF decoupling capacitor | 1 | ATtiny1614 VCC bypass |
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
