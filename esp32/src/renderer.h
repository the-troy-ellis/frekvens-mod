#pragma once
#include <stdint.h>
#include <stddef.h>   // size_t
#include "config.h"

// Logical pixel canvas: _w × _h, 1 byte per pixel (0–255 brightness). The
// canvas is the bounding box of the physical arrangement; panels can sit
// anywhere on it (rows, squares, L-shapes, gaps for non-LED modules). Chain
// position k displays the 16×16 region at _layout[k], through its rotation —
// all geometry lives here, the chain protocol and ATtiny firmware never see it.

class Renderer {
public:
    // w/h: canvas size in px (clamped to PANEL_* .. CANVAS_MAX_*).
    // layout: placement per chain position (MAX_PANELS entries; may be null →
    // default vertical stack). numPanels clamped to 1..MAX_PANELS — this ctor
    // is the choke point that keeps buildChainPacket inside its packet buffer.
    Renderer(uint16_t w, uint16_t h, const PanelPlacement* layout, uint8_t numPanels);
    ~Renderer();

    uint8_t  numPanels() const { return _numPanels; }
    uint16_t width()     const { return _w; }
    uint16_t height()    const { return _h; }

    // Direct pixel access (brightness 0–255)
    void    setPixel(int x, int y, uint8_t brightness);
    uint8_t getPixel(int x, int y) const;

    // Read-only access to the whole canvas (row-major, width() bytes per row,
    // height() rows) — lets the WS preview broadcast pass the buffer directly.
    const uint8_t* canvas() const { return _canvas; }

    // Optional gamma LUT (256 entries; nullptr = linear). Applied in
    // buildFrameForPanel — at the wire, not in setPixel — so effects that
    // read-modify-write the canvas (fades, trails) don't compound the curve,
    // and the canvas/preview always hold the intended linear values.
    void setGamma(const uint8_t* lut) { _gamma = lut; }

    void clear();

    // Text rendering using the built-in 5×7 font (brightness 0–255). Draws a
    // single glyph — tickDisplay's scroll loop calls these once per character.
    void drawChar(int x, int y, char c, uint8_t brightness);
    void drawCharVertical(int x, int y, char c, uint8_t brightness);

    // Blit an image into the canvas at (ox, oy).
    // data: row-major, 1 byte per pixel (0–255), w×h pixels.
    void blitImage(int ox, int oy, const uint8_t* data, int w, int h);

    // Build the Frekvens wire-format bit-plane buffer for one chain position:
    // samples the canvas at _layout[panelIdx] through its rotation.
    // out must point to FRAME_BYTES (256) bytes of writable storage.
    void buildFrameForPanel(uint8_t panelIdx, uint8_t* out) const;

    // Zone mode: write a native-oriented 16x16 buffer (as if rot=0) into the
    // shared canvas at panelIdx's placement, through the SAME rotation used
    // when sampling in buildFrameForPanel — the exact forward counterpart of
    // that sampling. So a zone's content, rendered upright into its own small
    // Renderer, appears upright on the physical panel regardless of its
    // mounting rotation once buildFrameForPanel later samples it back out.
    void blitPanelZone(uint8_t panelIdx, const uint8_t* zoneBuf16x16);

    // Build a complete chain packet for all panels.
    // Format: for each panel: magic + CMD_FRAME + 256 bytes + CRC8
    //         then: magic + CMD_SHOW
    // Returns the number of bytes written to out.
    // out must be at least MAX_PANELS * (3 + FRAME_BYTES + 1) + 3 bytes.
    size_t buildChainPacket(uint8_t* out) const;

private:
    uint16_t _w, _h;
    uint8_t  _numPanels;
    uint8_t* _canvas;                    // _w * _h bytes (8-bit per pixel)
    PanelPlacement _layout[MAX_PANELS];
    const uint8_t* _gamma = nullptr;     // optional 256-entry LUT, applied at serialisation

    static uint8_t crc8(const uint8_t* data, size_t len);
};
