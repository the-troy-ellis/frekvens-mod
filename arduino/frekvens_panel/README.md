# Frekvens Panel — Arduino IDE sketch

Arduino IDE version of the ATtiny1614 panel firmware. This is a direct copy of
the PlatformIO sources in `../../attiny1614/src/` — `main.cpp` became
`frekvens_panel.ino` (it already uses `setup()`/`loop()`). The `.cpp`/`.h` files
in this folder are compiled automatically by the IDE.

Keep this in sync with `attiny1614/src/` if you change the firmware there.

## Tools menu settings

| Setting | Value |
|---|---|
| Board | ATtiny3224/1624/1614/… (megaTinyCore, **non-Optiboot**) |
| Chip | **ATtiny1614** (defaults to 3224 — must set) |
| Clock | **10 MHz internal** (in spec at 3.3V; 20 MHz needs 4.5V) |
| BOD Voltage Level | **2.6V** |
| BOD Mode when Active | **Enabled** |
| Startup Time | **64 ms** |
| millis()/micros() Timer | **Disabled** (frees TCB0 for the /OE brightness PWM) |
| Programmer | SerialUPDI - 230400 baud (or 57600 if flaky) |
| Port | your `/dev/cu.usbserial-*` adapter |

## Flashing

1. **Tools → Burn Bootloader** once (writes the clock + BOD fuses).
2. **Sketch → Upload** to flash the firmware.

## Notes

- The clock is owned by the megaTinyCore "10 MHz internal" board setting — the
  firmware has no `clock_init()`. The UART baud divisor is derived from `F_CPU`
  in `uart.cpp`, so 1 Mbaud stays correct at whatever clock is selected.
- On power-up the firmware runs a self-test (fade + pixel walk) until the ESP32
  sends its first frame, then switches to live mode.
- A watchdog (~1 s) auto-recovers from any hang; BOD holds reset until VCC is
  stable for a clean cold-start.
