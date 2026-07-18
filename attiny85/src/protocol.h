#pragma once
#include <stdint.h>

// Packet framing
static const uint8_t PROTO_MAGIC_0   = 0xFE;
static const uint8_t PROTO_MAGIC_1   = 0xED;

// Commands
static const uint8_t CMD_FRAME       = 0x01;  // followed by 128 bytes of bit-plane data
static const uint8_t CMD_SHOW        = 0xFF;  // trigger buffer swap
static const uint8_t CMD_BRIGHTNESS  = 0x02;  // 1-byte global brightness scale 0-255
static const uint8_t CMD_CLEAR       = 0x03;  // clear display immediately

// Frame dimensions
static const uint8_t PANEL_WIDTH     = 16;
static const uint8_t PANEL_HEIGHT    = 16;
static const uint8_t BITPLANES       = 4;     // 16 brightness levels
static const uint8_t PLANE_BYTES     = 32;    // 256 bits per plane
static const uint8_t FRAME_BYTES     = 128;   // BITPLANES * PLANE_BYTES

// Bit-plane scan weights for PWM (relative on-time: 1:2:4:8)
// Plane 0 = LSB, shown least; Plane 3 = MSB, shown most
static const uint8_t PLANE_WEIGHT[BITPLANES] = {1, 2, 4, 8};
static const uint8_t WEIGHT_TOTAL = 15;  // sum of weights
