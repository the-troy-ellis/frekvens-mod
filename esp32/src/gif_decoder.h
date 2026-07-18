#pragma once
#include <stdint.h>

// .anim file format (written by GifDecoder, read by playback engine):
//
//   [4]  magic: "ANIM"
//   [1]  target_w   (should equal PANEL_WIDTH = 16)
//   [1]  target_h   (should equal numPanels * PANEL_HEIGHT)
//   [2]  num_frames (uint16_t LE)
//   [num_frames * frame_bytes]  pixel data (written first so we can stream)
//       frame_bytes = target_w * target_h  (8-bit grayscale, 1 byte per pixel)
//   [num_frames * 2]  frame_delays_ms (uint16_t LE each, appended last)
//
// Delay offset = sizeof(AnimHeader) + num_frames * frame_bytes

struct AnimHeader {
    char     magic[4];    // "ANIM"
    uint8_t  target_w;
    uint8_t  target_h;
    uint16_t num_frames;
    // followed by the pixel data (num_frames × frame_bytes), then the delay table
    // (num_frames × uint16_t). See the file-format comment at the top of this header.
};

class GifDecoder {
public:
    // Decode a GIF from LittleFS, write .anim to outputPath.
    // Frames are scaled (nearest-neighbour) to targetW × targetH.
    // invert: if true, each grey level is flipped (v → 255 - v) so dark/light swap,
    //   matching the Image tab's invert option.
    // levels: if 2..255, posterise each pixel to that many evenly-spaced grey levels
    //   (spanning the full 0..255 range) so a low-contrast animation collapses into a
    //   few clearly-distinct shades. 0 (or >=256) leaves the full 8-bit range intact.
    // Returns number of frames decoded, or -1 on parse error.
    static int decode(const char* inputPath, const char* outputPath,
                      uint8_t targetW, uint8_t targetH,
                      bool invert = false, uint16_t levels = 0);

private:
    struct ColorTable {
        uint8_t  r[256], g[256], b[256];
        uint16_t size;  // number of entries (up to 256 — must not be uint8_t, which
                        // would truncate a full 256-entry table to 0)
    };

    struct GraphicControl {
        uint16_t delayCs;   // hundredths of a second
        uint8_t  transparentIdx;
        bool     hasTransparency;
        uint8_t  disposalMethod;
    };

    // LZW code table entry
    struct LZWEntry {
        uint16_t prefix;  // 0xFFFF = root
        uint8_t  suffix;
    };

    // Sub-block aware bit reader
    struct BitReader {
        uint8_t  subLeft   = 0;
        uint32_t buf       = 0;
        int      bitsLeft  = 0;

        bool     refillSub(void* f);
        uint16_t read(int n, void* f);
    };

    static bool     readColorTable(void* f, ColorTable& ct, uint8_t sizeBits);
    static bool     skipExtension(void* f);
    // Widths/heights/offsets are uint16_t: GIF dimensions routinely exceed 255
    // (this is a 480×480 example). uint8_t here truncated 480→224 and corrupted
    // every frame. numPixels is uint32_t because w*h can exceed 65535.
    static bool     decodeFrame(void* f, const ColorTable& gct,
                                uint16_t frameW, uint16_t frameH,
                                uint16_t offsetX, uint16_t offsetY,
                                uint16_t canvasW, uint16_t canvasH,
                                const GraphicControl& gc,
                                uint8_t* canvas);
    static bool     lzwDecode(void* f, uint8_t minCodeSize,
                               uint8_t* output, uint32_t numPixels);
    static uint8_t  toGray8(uint8_t r, uint8_t g, uint8_t b);
    static void     scaleToTarget(const uint8_t* src, uint16_t sw, uint16_t sh,
                                  uint8_t* dst, uint8_t dw, uint8_t dh);
};
