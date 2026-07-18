#pragma once
#include <stdint.h>

// Wire protocol — must match esp32/src/protocol_shared.h exactly
static const uint8_t  PROTO_MAGIC_0   = 0xFE;
static const uint8_t  PROTO_MAGIC_1   = 0xED;
static const uint8_t  CMD_FRAME       = 0x01;
static const uint8_t  CMD_SHOW        = 0xFF;
static const uint8_t  CMD_BRIGHTNESS  = 0x02;
static const uint8_t  CMD_CLEAR       = 0x03;

// Display geometry
static const uint8_t  PANEL_W         = 16;
static const uint8_t  PANEL_H         = 16;

// 8-bit depth: 8 bit-planes, 256 brightness levels
static const uint8_t  BITPLANES       = 8;
static const uint8_t  PLANE_BYTES     = 32;    // 256 pixels / 8 bits = 32 bytes per plane
static const uint16_t FRAME_BYTES     = 256;   // BITPLANES * PLANE_BYTES

// Bit-plane weights for PWM (relative on-time per plane, 2^p)
// Total weight = 255. Visual frame = 255 scan ticks.
static const uint8_t PLANE_WEIGHT[BITPLANES] = {1, 2, 4, 8, 16, 32, 64, 128};
