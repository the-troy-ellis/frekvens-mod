#include "gif_decoder.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <string.h>
#include <stdlib.h>

// Big buffers (full-resolution GIF frames — 480×480 ≈ 225 KB each) go to PSRAM,
// which the XIAO ESP32-S3 has 8 MB of and the board enables via BOARD_HAS_PSRAM.
// ps_malloc returns null if PSRAM is absent/full, so fall back to internal heap
// (fine for small GIFs). Pair with bigFree, which frees either kind.
static void* bigAlloc(size_t n) {
    void* p = ps_malloc(n);
    if (!p) p = malloc(n);
    return p;
}
static void* bigCalloc(size_t n) {
    void* p = bigAlloc(n);
    if (p) memset(p, 0, n);
    return p;
}
#define bigFree(p) free(p)

// Helpers to read typed values from a File*
static uint8_t  rf8 (File& f)             { return (uint8_t)f.read(); }
static uint16_t rf16(File& f)             { uint16_t v = rf8(f); v |= (uint16_t)rf8(f) << 8; return v; }
static void     rfn (File& f, void* d, size_t n) { f.read((uint8_t*)d, n); }
static void     skip(File& f, size_t n)   { for (size_t i = 0; i < n; i++) f.read(); }

// ---- Bit reader (reads across GIF sub-blocks) ----

bool GifDecoder::BitReader::refillSub(void* fp) {
    File& f = *reinterpret_cast<File*>(fp);
    if (subLeft == 0) {
        subLeft = rf8(f);
        if (subLeft == 0) return false;  // block terminator
    }
    buf |= (uint32_t)rf8(f) << bitsLeft;
    bitsLeft += 8;
    subLeft--;
    return true;
}

uint16_t GifDecoder::BitReader::read(int n, void* fp) {
    while (bitsLeft < n) refillSub(fp);
    uint16_t v = buf & ((1u << n) - 1);
    buf >>= n;
    bitsLeft -= n;
    return v;
}

// Drain remaining sub-block bytes after a LZW stream ends
static void drainSubBlocks(File& f) {
    for (;;) {
        uint8_t sz = rf8(f);
        if (sz == 0) break;
        skip(f, sz);
    }
}

// ---- Color table ----

bool GifDecoder::readColorTable(void* fp, ColorTable& ct, uint8_t sizeBits) {
    File& f = *reinterpret_cast<File*>(fp);
    ct.size = 1 << (sizeBits + 1);
    for (int i = 0; i < ct.size; i++) {
        ct.r[i] = rf8(f);
        ct.g[i] = rf8(f);
        ct.b[i] = rf8(f);
    }
    return true;
}

// ---- Extension block skip ----

bool GifDecoder::skipExtension(void* fp) {
    File& f = *reinterpret_cast<File*>(fp);
    drainSubBlocks(f);
    return true;
}

// ---- Grayscale conversion ----

uint8_t GifDecoder::toGray8(uint8_t r, uint8_t g, uint8_t b) {
    // ITU-R BT.601 luminance. uint32_t — the accumulator exceeds 16 bits
    // (255*299 alone > 65535), so a uint16_t here would overflow.
    uint32_t lum = (uint32_t)r * 299 + (uint32_t)g * 587 + (uint32_t)b * 114;
    return (uint8_t)(lum / 1000);  // 0-255 (8-bit grayscale)
}

// ---- Nearest-neighbour scale ----

void GifDecoder::scaleToTarget(const uint8_t* src, uint16_t sw, uint16_t sh,
                                uint8_t* dst, uint8_t dw, uint8_t dh) {
    for (int dy = 0; dy < dh; dy++) {
        int sy = (dy * sh) / dh;
        for (int dx = 0; dx < dw; dx++) {
            int sx = (dx * sw) / dw;
            dst[dy * dw + dx] = src[(size_t)sy * sw + sx];
        }
    }
}

// ---- LZW decompressor ----

bool GifDecoder::lzwDecode(void* fp, uint8_t minCodeSize,
                            uint8_t* output, uint32_t numPixels) {
    File& f = *reinterpret_cast<File*>(fp);

    const uint16_t CLEAR = 1 << minCodeSize;
    const uint16_t EOI   = CLEAR + 1;

    static LZWEntry table[4096];
    // Init root entries (indices 0..CLEAR-1)
    for (uint16_t i = 0; i < CLEAR; i++) { table[i].prefix = 0xFFFF; table[i].suffix = (uint8_t)i; }

    int      codeSize = minCodeSize + 1;
    uint16_t nextCode = EOI + 1;
    uint16_t prevCode = 0xFFFF;
    uint32_t outIdx   = 0;   // may exceed 65535 for large frames (480×480 = 230400)

    BitReader br = {};

    // Decode a code to a stack, output in correct order
    auto emit = [&](uint16_t code) -> bool {
        uint8_t stack[4096];
        int sp = 0;
        uint16_t c = code;
        while (c != 0xFFFF) {
            if (sp >= 4096) return false;
            stack[sp++] = table[c].suffix;
            c = table[c].prefix;
        }
        for (int i = sp - 1; i >= 0; i--) {
            if (outIdx < numPixels) output[outIdx++] = stack[i];
        }
        return true;
    };

    auto firstPixel = [&](uint16_t code) -> uint8_t {
        while (table[code].prefix != 0xFFFF) code = table[code].prefix;
        return table[code].suffix;
    };

    uint32_t codeCount = 0;
    for (;;) {
        if (codeSize > 12) break;
        // Big frames decode hundreds of thousands of codes. This runs in the async
        // web-server task; delay(1) is a real vTaskDelay that lets the idle task run,
        // so the task-watchdog doesn't fire on a multi-second decode (yield() alone
        // never yields to the idle task and wouldn't prevent that panic).
        if ((++codeCount & 0x0FFF) == 0) delay(1);
        uint16_t code = br.read(codeSize, fp);

        if (code == CLEAR) {
            codeSize = minCodeSize + 1;
            nextCode = EOI + 1;
            prevCode = 0xFFFF;
            continue;
        }
        if (code == EOI) break;

        if (code < nextCode) {
            emit(code);
            if (prevCode != 0xFFFF && nextCode < 4096) {
                table[nextCode].prefix = prevCode;
                table[nextCode].suffix = firstPixel(code);
                nextCode++;
            }
        } else if (code == nextCode && prevCode != 0xFFFF) {
            // K+K case
            uint8_t fp_val = firstPixel(prevCode);
            emit(prevCode);
            if (outIdx < numPixels) output[outIdx++] = fp_val;
            if (nextCode < 4096) {
                table[nextCode].prefix = prevCode;
                table[nextCode].suffix = fp_val;
                nextCode++;
            }
        } else {
            break;  // corrupt stream
        }

        prevCode = code;
        if (nextCode == (uint16_t)(1 << codeSize) && codeSize < 12) codeSize++;
    }

    drainSubBlocks(f);
    return (outIdx == numPixels);
}

// ---- Frame decoder ----

bool GifDecoder::decodeFrame(void* fp, const ColorTable& gct,
                              uint16_t frameW, uint16_t frameH,
                              uint16_t offsetX, uint16_t offsetY,
                              uint16_t canvasW, uint16_t canvasH,
                              const GraphicControl& gc,
                              uint8_t* canvas) {
    File& f = *reinterpret_cast<File*>(fp);

    size_t   frameCount  = (size_t)frameW * frameH;
    uint8_t* framePixels = (uint8_t*)bigAlloc(frameCount);
    if (!framePixels) return false;

    // Local color table?
    uint8_t packed = rf8(f);
    bool    hasLCT  = packed & 0x80;
    bool    isInterlaced = packed & 0x40;
    uint8_t lctSizeBits = packed & 0x07;

    ColorTable lct;
    if (hasLCT) readColorTable(fp, lct, lctSizeBits);
    const ColorTable& ct = hasLCT ? lct : gct;

    uint8_t minCodeSize = rf8(f);
    bool ok = lzwDecode(fp, minCodeSize, framePixels, (uint32_t)frameCount);

    if (ok) {
        // Deinterlace if needed
        if (isInterlaced) {
            uint8_t* tmp = (uint8_t*)bigAlloc(frameCount);
            if (tmp) {
                const uint8_t passes[4] = {0, 4, 2, 1};
                const uint8_t steps[4]  = {8, 8, 4, 2};
                int src = 0;
                for (int pass = 0; pass < 4; pass++) {
                    for (int row = passes[pass]; row < frameH; row += steps[pass]) {
                        memcpy(tmp + (size_t)row * frameW, framePixels + (size_t)src * frameW, frameW);
                        src++;
                    }
                }
                memcpy(framePixels, tmp, frameCount);
                bigFree(tmp);
            }
        }

        // Map indexed → grayscale, blit into canvas
        for (int y = 0; y < frameH; y++) {
            for (int x = 0; x < frameW; x++) {
                uint8_t idx = framePixels[(size_t)y * frameW + x];
                if (gc.hasTransparency && idx == gc.transparentIdx) continue;
                int cx = offsetX + x, cy = offsetY + y;
                if (cx < canvasW && cy < canvasH)
                    canvas[(size_t)cy * canvasW + cx] = toGray8(ct.r[idx], ct.g[idx], ct.b[idx]);
            }
        }
    }

    bigFree(framePixels);
    return ok;
}

// ---- Top-level decoder ----

int GifDecoder::decode(const char* inputPath, const char* outputPath,
                       uint8_t targetW, uint8_t targetH, bool invert, uint16_t levels) {
    // Posterise flag: only quantise when a sensible level count was asked for.
    bool doLevels = (levels >= 2 && levels < 256);
    File f = LittleFS.open(inputPath, "r");
    if (!f) return -1;

    // Header
    char sig[7] = {};
    rfn(f, sig, 6);
    if (memcmp(sig, "GIF87a", 6) != 0 && memcmp(sig, "GIF89a", 6) != 0) { f.close(); return -1; }

    // Logical Screen Descriptor
    uint16_t canvasW = rf16(f);
    uint16_t canvasH = rf16(f);
    uint8_t  screenPacked = rf8(f);
    rf8(f);  // bg color index (ignored)
    rf8(f);  // pixel aspect ratio (ignored)

    bool hasGCT = screenPacked & 0x80;
    uint8_t gctSizeBits = screenPacked & 0x07;

    ColorTable gct = {};
    if (hasGCT) readColorTable(&f, gct, gctSizeBits);

    // Sanity cap: reject absurd dimensions before allocating (guards against a
    // corrupt header claiming e.g. 65535×65535 ≈ 4 GB).
    size_t canvasBytes = (size_t)canvasW * canvasH;
    if (canvasW == 0 || canvasH == 0 || canvasBytes > (size_t)4 * 1024 * 1024) {
        f.close(); return -1;
    }

    // Canvas buffer (grayscale, 1 byte per pixel). Full-res frames are large
    // (480×480 ≈ 225 KB) so this goes to PSRAM via bigCalloc.
    uint8_t* canvas = (uint8_t*)bigCalloc(canvasBytes);
    if (!canvas) { f.close(); return -1; }

    // We need to write the header after we know num_frames,
    // so collect frame data in a second pass via a temp file.
    File out = LittleFS.open(outputPath, "w");
    if (!out) { bigFree(canvas); f.close(); return -1; }

    // Reserve space for header (written at end)
    uint16_t maxFrames = 512;
    uint16_t* delays = (uint16_t*)malloc(maxFrames * sizeof(uint16_t));
    if (!delays) { bigFree(canvas); f.close(); out.close(); return -1; }

    uint16_t numFrames = 0;
    size_t   frameBytes = (size_t)targetW * targetH;   // 8-bit: 1 byte per pixel

    // Track every output write: LittleFS filling up mid-decode used to be
    // invisible — frames were silently dropped but the header still claimed the
    // full count, and the upload reported success for a broken file.
    bool ioOk = true;

    // Placeholder header (overwritten at end)
    AnimHeader hdr = {{'A','N','I','M'}, targetW, targetH, 0};   // no null in char[4]
    if (out.write((uint8_t*)&hdr, sizeof(hdr)) != sizeof(hdr)) ioOk = false;

    GraphicControl gc = {};
    uint8_t prevDisposal = 0;
    uint8_t* prevCanvas = nullptr;

    for (;;) {
        uint8_t introducer = rf8(f);
        if (!f.available() || introducer == 0x3B) break;  // trailer

        if (introducer == 0x21) {
            // Extension
            uint8_t label = rf8(f);
            if (label == 0xF9) {
                // Graphic Control Extension
                rf8(f);  // block size (always 4)
                uint8_t gcPacked = rf8(f);
                gc.delayCs           = rf16(f);
                gc.transparentIdx    = rf8(f);
                gc.hasTransparency   = gcPacked & 0x01;
                gc.disposalMethod    = (gcPacked >> 2) & 0x07;
                rf8(f);  // block terminator
            } else {
                skipExtension(&f);
            }
        } else if (introducer == 0x2C) {
            // Image Descriptor
            uint16_t ox  = rf16(f);
            uint16_t oy  = rf16(f);
            uint16_t fw  = rf16(f);
            uint16_t fh  = rf16(f);

            // Handle disposal
            if (prevDisposal == 2) {
                memset(canvas, 0, canvasBytes);
            } else if (prevDisposal == 3 && prevCanvas) {
                memcpy(canvas, prevCanvas, canvasBytes);
            }
            if (gc.disposalMethod == 3) {
                if (!prevCanvas) prevCanvas = (uint8_t*)bigAlloc(canvasBytes);
                if (prevCanvas) memcpy(prevCanvas, canvas, canvasBytes);
            }
            prevDisposal = gc.disposalMethod;

            decodeFrame(&f, gct, fw, fh, ox, oy, canvasW, canvasH, gc, canvas);

            // Scale canvas → target and write the frame directly (8-bit, 1 byte/pixel)
            uint8_t* scaled = (uint8_t*)malloc(frameBytes);
            if (scaled) {
                scaleToTarget(canvas, canvasW, canvasH,
                              scaled, targetW, targetH);
                // Posterise first (snap to N evenly-spaced levels spanning 0..255),
                // then invert, so the reduced levels map cleanly onto the panel.
                if (doLevels) {
                    for (size_t i = 0; i < frameBytes; i++) {
                        uint16_t bucket = (uint16_t)scaled[i] * levels / 256;   // 0..levels-1
                        if (bucket > levels - 1) bucket = levels - 1;
                        scaled[i] = (uint8_t)(bucket * 255 / (levels - 1));      // back to full scale
                    }
                }
                if (invert)
                    for (size_t i = 0; i < frameBytes; i++) scaled[i] = 255 - scaled[i];
                if (out.write(scaled, frameBytes) != frameBytes) ioOk = false;
                if (numFrames < maxFrames) {
                    uint32_t ms = (uint32_t)gc.delayCs * 10;   // cs → ms
                    delays[numFrames] = ms > 65535 ? 65535 : (uint16_t)ms;  // no wrap
                }
                numFrames++;
            }
            free(scaled);

            gc = {};  // reset graphic control for next frame
        }

        delay(1);  // per-block vTaskDelay — feeds the idle-task watchdog
        if (numFrames >= maxFrames) break;
    }

    // Append all delays at end of file (after pixel data)
    size_t delayBytes = (size_t)numFrames * sizeof(uint16_t);
    if (out.write((uint8_t*)delays, delayBytes) != delayBytes) ioOk = false;

    // Seek back to position 0 and patch the header (only 8 bytes, does not touch pixel data)
    if (!out.seek(0)) ioOk = false;
    hdr.num_frames = numFrames;
    if (out.write((uint8_t*)&hdr, sizeof(hdr)) != sizeof(hdr)) ioOk = false;

    free(delays); bigFree(canvas); bigFree(prevCanvas);
    f.close(); out.close();
    // On any write failure the file on disk is incomplete/mislabeled — report
    // failure so the caller removes it instead of announcing a frame count.
    return ioOk ? numFrames : -1;
}
