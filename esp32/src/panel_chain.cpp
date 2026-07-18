#include "panel_chain.h"
#include "config.h"
#include "protocol_shared.h"
#include <Arduino.h>
#include <HardwareSerial.h>

// Max packet: numPanels × (2 magic + 1 cmd + FRAME_BYTES + 1 crc) + 3 show
// With FRAME_BYTES=256, MAX_PANELS=4: 4×260 + 3 = 1043 bytes → ~10.4 ms at 1 Mbaud (~95 fps)
#define MAX_PACKET_SIZE (MAX_PANELS * (3 + FRAME_BYTES + 1) + 3)

static HardwareSerial chainSerial(CHAIN_UART_NUM);

PanelChain::PanelChain(uint8_t numPanels)
    : _numPanels(numPanels < 1 ? 1 : (numPanels > MAX_PANELS ? MAX_PANELS : numPanels))
{}

void PanelChain::begin() {
    // TX ring buffer (must be set before begin()). Without one, write() blocks as
    // soon as the 128-byte hardware FIFO fills — a full 4-panel frame (1043 bytes
    // at 250 kbaud) stalled loop() for ~42 ms per flush, starving WS/MQTT/OTA.
    // 2048 holds the largest packet plus stragglers; the UART ISR drains it in
    // the background. Sustained worst case (4 panels @ 20 fps ≈ 20.9 KB/s) stays
    // under the 25 KB/s line rate, so the buffer can't grow without bound.
    chainSerial.setTxBufferSize(2048);
    chainSerial.begin(CHAIN_UART_BAUD, SERIAL_8N1, PIN_UART_RX, PIN_UART_TX);
}

void PanelChain::flush(const Renderer& renderer) {
    static uint8_t packet[MAX_PACKET_SIZE];
    size_t len = renderer.buildChainPacket(packet);
    sendBytes(packet, len);
}

void PanelChain::clearAll() {
    uint8_t pkt[3] = { PROTO_MAGIC_0, PROTO_MAGIC_1, CMD_CLEAR };
    sendBytes(pkt, sizeof(pkt));
}

void PanelChain::setBrightness(uint8_t b) {
    uint8_t pkt[4] = { PROTO_MAGIC_0, PROTO_MAGIC_1, CMD_BRIGHTNESS, b };
    sendBytes(pkt, sizeof(pkt));
}

void PanelChain::sendBytes(const uint8_t* data, size_t len) {
    // Queued into the TX ring buffer and sent by the UART ISR — no flush():
    // blocking until the last bit left the wire gained nothing and cost the
    // whole transmission time every frame.
    chainSerial.write(data, len);
}
