#pragma once
#include "protocol_shared.h"   // BITPLANES / PLANE_BYTES / FRAME_BYTES + protocol consts

// --- Hardware (Seeed XIAO ESP32-S3) ---
// XIAO ESP32-S3 exposes GPIOs by "D" number; these are the actual GPIO numbers.
//   D0 = GPIO1, D1 = GPIO2, D2 = GPIO3, ...  (D6/GPIO43, D7/GPIO44 are UART0)
// We route a spare UART instance to D0/D1 so it doesn't clash with the USB
// console or the default UART0 pins.
#define PIN_UART_TX         1     // D0 / GPIO1 → ATtiny1614 #0 RXD (pin 6, PB3)
#define PIN_UART_RX         2     // D1 / GPIO2 ← ATtiny1614 #0 TXD (pin 7, diagnostics)
#define CHAIN_UART_BAUD     250000   // 250 kbaud — comfortable RX margin for the
                                     // ATtiny at 10 MHz (1 Mbaud overran its ISR on
                                     // 256-byte frames). ~96fps @ 1 panel, ~24fps @ 4.
#define CHAIN_UART_NUM      1     // UART1 (UART0 is the default console)

// Maximum panels in chain
#define MAX_PANELS          4

// Default panel count (can be changed via web UI / stored in NVS)
#define DEFAULT_NUM_PANELS  1

// --- Display ---
// BITPLANES / PLANE_BYTES / FRAME_BYTES come from protocol_shared.h (shared with
// the ATtiny). Only the panel geometry is defined here.
#define PANEL_WIDTH         16
#define PANEL_HEIGHT        16

// --- Panel arrangement ---
// Panels can be arranged arbitrarily in 2D (rows, squares, L-shapes, gaps where
// a speaker module sits between panels). The logical canvas is the bounding box
// of all placements; chain position k samples the canvas at layout[k]. Pixel
// (not cell) coordinates, because other Frekvens-line modules aren't exact
// panel multiples. The caps bound canvas memory (128x128 = 16 KB max).
#define CANVAS_MAX_W        128
#define CANVAS_MAX_H        128

#include <stdint.h>
// Plain aggregate (no default member initializers — the core builds as gnu++11,
// where that combination breaks brace-init).
struct PanelPlacement {
    int16_t x;      // top-left of this panel's 16x16 cell, canvas px
    int16_t y;
    uint8_t rot;    // physical panel rotation, 0..3 = 0/90/180/270 deg CW
};

// --- Networking ---
#define HOSTNAME            "frekvens"
#define AP_SSID             "Frekvens-Setup"
#define AP_PASSWORD         "frekvens1"
#define OTA_PASSWORD        "frekvens-ota"

// --- MQTT ---
#define MQTT_TOPIC_BASE         "frekvens"
#define MQTT_TOPIC_STATE        MQTT_TOPIC_BASE "/state"
#define MQTT_TOPIC_SET          MQTT_TOPIC_BASE "/set"
#define MQTT_TOPIC_DISPLAY_SET  MQTT_TOPIC_BASE "/display/set"
#define MQTT_TOPIC_AVAILABILITY MQTT_TOPIC_BASE "/availability"
// Retained JSON arrays of stored filenames, e.g. ["cat.raw","dog.raw"], so HA
// automations can keep an input_select's option list in sync automatically.
#define MQTT_TOPIC_IMAGES       MQTT_TOPIC_BASE "/images"
#define MQTT_TOPIC_ANIMS        MQTT_TOPIC_BASE "/anims"
// Native HA entities (text / select / number) — command topics HA writes to,
// state topics we publish (retained) so HA restores state on restart.
#define MQTT_TOPIC_TEXT_SET         MQTT_TOPIC_BASE "/text/set"
#define MQTT_TOPIC_TEXT_STATE       MQTT_TOPIC_BASE "/text/state"
#define MQTT_TOPIC_ANIM_SET         MQTT_TOPIC_BASE "/anim/set"
#define MQTT_TOPIC_ANIM_STATE       MQTT_TOPIC_BASE "/anim/state"
#define MQTT_TOPIC_SPEED_SET        MQTT_TOPIC_BASE "/speed/set"
#define MQTT_TOPIC_SPEED_STATE      MQTT_TOPIC_BASE "/speed/state"
#define MQTT_TOPIC_SCROLLMODE_SET   MQTT_TOPIC_BASE "/scroll_mode/set"
#define MQTT_TOPIC_SCROLLMODE_STATE MQTT_TOPIC_BASE "/scroll_mode/state"
// HA keeps the live sky condition current here (retained); the device tracks it
// when weather mode is set to "auto". Plain string payload, e.g. "rainy".
#define MQTT_TOPIC_WEATHER_SET      MQTT_TOPIC_BASE "/weather/set"
#define MQTT_DISCOVERY_PREFIX   "homeassistant"

// --- Storage ---
#define CONFIG_FILE         "/config.json"
#define ANIM_DIR            "/anims"
#define IMAGE_DIR           "/images"
