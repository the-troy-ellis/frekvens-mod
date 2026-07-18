#pragma once
#include <stdint.h>
#include <stddef.h>   // size_t
#include "renderer.h"

class PanelChain {
public:
    // numPanels: how many ATtiny85/Frekvens units in the chain (1–MAX_PANELS)
    explicit PanelChain(uint8_t numPanels);

    void begin();

    // Push current renderer frame to the chain over UART
    void flush(const Renderer& renderer);

    // Send CMD_CLEAR to all units
    void clearAll();

    // Send CMD_BRIGHTNESS to all units (0–255)
    void setBrightness(uint8_t b);

    uint8_t numPanels() const { return _numPanels; }

private:
    uint8_t _numPanels;

    void sendBytes(const uint8_t* data, size_t len);
};
