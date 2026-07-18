#include "renderer.h"
#include "protocol_shared.h"
#include <string.h>
#include <stdlib.h>
#include "font5x7.h"
#include "font5x7_vertical.h"

// --- Canvas: 1 byte per pixel (8-bit brightness, 0-255) ---

Renderer::Renderer(uint16_t w, uint16_t h, const PanelPlacement* layout, uint8_t numPanels)
    // Clamp to 1..MAX_PANELS. buildChainPacket() writes _numPanels × 260 bytes into
    // a MAX_PANELS-sized packet buffer, so an out-of-range count here is a buffer
    // overflow. This ctor is the single choke point that guarantees it can't happen.
    : _numPanels(numPanels < 1 ? 1 : (numPanels > MAX_PANELS ? MAX_PANELS : numPanels)) {
    _w = w < PANEL_WIDTH  ? PANEL_WIDTH  : (w > CANVAS_MAX_W ? CANVAS_MAX_W : w);
    _h = h < PANEL_HEIGHT ? PANEL_HEIGHT : (h > CANVAS_MAX_H ? CANVAS_MAX_H : h);
    for (uint8_t i = 0; i < MAX_PANELS; i++) {
        if (layout) _layout[i] = layout[i];
        else        _layout[i] = { 0, (int16_t)(i * PANEL_HEIGHT), 0 };   // legacy vertical stack
    }
    _canvas = (uint8_t*)calloc((size_t)_w * _h, 1);
}

Renderer::~Renderer() {
    free(_canvas);
}

void Renderer::setPixel(int x, int y, uint8_t brightness) {
    if (x < 0 || x >= _w || y < 0 || y >= _h) return;
    _canvas[(size_t)y * _w + x] = brightness;
}

uint8_t Renderer::getPixel(int x, int y) const {
    if (x < 0 || x >= _w || y < 0 || y >= _h) return 0;
    return _canvas[(size_t)y * _w + x];
}

void Renderer::clear() {
    memset(_canvas, 0, (size_t)_w * _h);
}

// --- Text rendering ---

void Renderer::drawChar(int x, int y, char c, uint8_t brightness) {
    if ((uint8_t)c < 32 || (uint8_t)c > 127) c = '?';
    const uint8_t* glyph = font5x7[(uint8_t)c - 32];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; row++) {
            if (bits & (1 << row)) setPixel(x + col, y + row, brightness);
        }
    }
}

// Vertical companion to drawChar: reads the pre-rotated font5x7_vertical table
// (7 columns x 5 rows — dimensions swapped from the normal 5x7 glyph) instead of
// rotating pixels at runtime.
void Renderer::drawCharVertical(int x, int y, char c, uint8_t brightness) {
    if ((uint8_t)c < 32 || (uint8_t)c > 127) c = '?';
    const uint8_t* glyph = font5x7_vertical[(uint8_t)c - 32];
    for (int col = 0; col < 7; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 5; row++) {
            if (bits & (1 << row)) setPixel(x + col, y + row, brightness);
        }
    }
}

void Renderer::blitImage(int ox, int oy, const uint8_t* data, int w, int h) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            setPixel(ox + x, oy + y, data[y * w + x]);
        }
    }
}

// --- Frekvens bit-plane serialisation ---
// Frekvens pixel addressing (from original FrekvensPanel::drawPixel):
//   if x > 7: addr = (x-8) + (y+16)*8
//   else:     addr =  x    +  y    *8
//   byte_in_plane = addr / 8
//   bit_in_byte   = addr % 8
//
// For each pixel (0-255 brightness), distribute its 8 bits across 8 planes.

void Renderer::buildFrameForPanel(uint8_t panelIdx, uint8_t* out) const {
    memset(out, 0, FRAME_BYTES);
    const PanelPlacement& pl = _layout[panelIdx];

    // (x, y) iterate the panel's NATIVE pixels (the addressing math below is in
    // native coordinates); the canvas is sampled at where that native pixel
    // physically sits after the panel's mounting rotation. A panel rotated 90°
    // clockwise has its native (x,y) at cell position (15-y, x), and so on —
    // so a correctly-configured rotation shows canvas content upright. The
    // __identify__ effect's top-edge tick makes a wrong rot value obvious.
    for (int y = 0; y < PANEL_HEIGHT; y++) {
        for (int x = 0; x < PANEL_WIDTH; x++) {
            int cx, cy;
            switch (pl.rot & 3) {
                case 1:  cx = pl.x + 15 - y; cy = pl.y + x;      break;   //  90° CW
                case 2:  cx = pl.x + 15 - x; cy = pl.y + 15 - y; break;   // 180°
                case 3:  cx = pl.x + y;      cy = pl.y + 15 - x; break;   // 270° CW
                default: cx = pl.x + x;      cy = pl.y + y;      break;   //   0°
            }
            uint8_t v = getPixel(cx, cy);          // off-canvas samples read 0
            if (_gamma) v = _gamma[v];             // perceptual curve at the wire only
            if (!v) continue;

            int xa = x, ya = y;
            if (xa > 7) { ya += 16; xa -= 8; }
            int addr  = xa + ya * 8;
            int byteI = addr / 8;
            int bitI  = addr % 8;

            for (int p = 0; p < BITPLANES; p++) {
                if (v & (1 << p)) {
                    out[(uint16_t)p * PLANE_BYTES + byteI] |= (1 << bitI);
                }
            }
        }
    }
}

void Renderer::blitPanelZone(uint8_t panelIdx, const uint8_t* zoneBuf16x16) {
    if (panelIdx >= MAX_PANELS) return;
    const PanelPlacement& pl = _layout[panelIdx];
    // Identical rotation mapping to buildFrameForPanel's sampling switch above —
    // this is deliberately the same math, not just similar: blit-then-sample
    // must be an exact round trip, or a zone's content would appear rotated
    // wrong on a mounted-sideways panel. (Verified by a native unit test that
    // blits then re-samples for all four rotations.)
    for (int y = 0; y < PANEL_HEIGHT; y++) {
        for (int x = 0; x < PANEL_WIDTH; x++) {
            int cx, cy;
            switch (pl.rot & 3) {
                case 1:  cx = pl.x + 15 - y; cy = pl.y + x;      break;
                case 2:  cx = pl.x + 15 - x; cy = pl.y + 15 - y; break;
                case 3:  cx = pl.x + y;      cy = pl.y + 15 - x; break;
                default: cx = pl.x + x;      cy = pl.y + y;      break;
            }
            setPixel(cx, cy, zoneBuf16x16[y * PANEL_WIDTH + x]);
        }
    }
}

// CRC-8 Dallas/Maxim (poly 0x31) — must match ATtiny1614 firmware
uint8_t Renderer::crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0x00;
    while (len--) {
        uint8_t b = *data++;
        for (int i = 0; i < 8; i++) {
            if ((crc ^ b) & 0x80) crc = (crc << 1) ^ 0x31;
            else                   crc = (crc << 1);
            b <<= 1;
        }
    }
    return crc;
}

size_t Renderer::buildChainPacket(uint8_t* out) const {
    uint8_t* p = out;

    for (uint8_t panel = 0; panel < _numPanels; panel++) {
        *p++ = PROTO_MAGIC_0;
        *p++ = PROTO_MAGIC_1;
        *p++ = CMD_FRAME;

        uint8_t frame[FRAME_BYTES];
        buildFrameForPanel(panel, frame);
        memcpy(p, frame, FRAME_BYTES);
        p += FRAME_BYTES;
        *p++ = crc8(frame, FRAME_BYTES);
    }

    // Single CMD_SHOW triggers synchronised buffer swap on all units
    *p++ = PROTO_MAGIC_0;
    *p++ = PROTO_MAGIC_1;
    *p++ = CMD_SHOW;

    return (size_t)(p - out);
}
