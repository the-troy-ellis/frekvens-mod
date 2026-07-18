#pragma once
#include <stdint.h>

// Shared between ESP32 firmware and ATtiny1614 firmware.
// Any change here must be reflected in attiny1614/src/protocol.h.

static const uint8_t  PROTO_MAGIC_0   = 0xFE;
static const uint8_t  PROTO_MAGIC_1   = 0xED;
static const uint8_t  CMD_FRAME       = 0x01;
static const uint8_t  CMD_SHOW        = 0xFF;
static const uint8_t  CMD_BRIGHTNESS  = 0x02;
static const uint8_t  CMD_CLEAR       = 0x03;

// 8-bit depth — 256 brightness levels per pixel
static const uint8_t  BITPLANES       = 8;
static const uint8_t  PLANE_BYTES     = 32;
static const uint16_t FRAME_BYTES     = 256;   // BITPLANES * PLANE_BYTES
