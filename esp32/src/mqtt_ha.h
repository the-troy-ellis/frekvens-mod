#pragma once
#include <stdint.h>
#include <functional>

// Callbacks
using MqttDisplayCallback = std::function<void(const char* json)>;
using MqttStateCallback   = std::function<void(bool on, uint8_t brightness)>;
// Native HA entity commands (text/select/number). kind is one of "text",
// "anim", "speed", "scrollmode"; value is the raw payload string.
using MqttSimpleCallback  = std::function<void(const char* kind, const char* value)>;

class MqttHA {
public:
    MqttHA() = default;

    void begin(const char* broker, uint16_t port,
               const char* user, const char* pass,
               const char* deviceName);

    // Call from loop() — handles reconnect and message dispatch
    void loop();

    // Publish current state (called after any state change)
    void publishState(bool on, uint8_t brightness);

    // Publish HA autodiscovery payloads for light entity
    void publishDiscovery();

    // Publish the current stored image/animation filenames as retained JSON arrays
    // (frekvens/images, frekvens/anims), so an HA automation can keep an
    // input_select's options in sync without manual upkeep. Called on every
    // (re)connect and whenever main.cpp signals a file changed. Also republishes
    // the HA animation-select discovery, whose options list must track the files.
    void publishFileLists();

    // Retained state for the native HA entities (text / anim select / speed
    // number / scroll-mode select). animName should be "(none)" when nothing
    // user-selectable is playing.
    void publishModeState(const char* text, const char* animName,
                          uint16_t scrollMs, const char* scrollMode);

    void onDisplaySet(MqttDisplayCallback cb) { _displayCb = cb; }
    void onStateSet(MqttStateCallback cb)      { _stateCb   = cb; }
    void onSimpleSet(MqttSimpleCallback cb)    { _simpleCb  = cb; }

    bool connected() const;

    // Called by the static PubSubClient trampoline in mqtt_ha.cpp — must be public.
    void onMessage(const char* topic, const uint8_t* payload, unsigned int len);

private:
    void reconnect();

    char _broker[64]     = {};
    char _user[32]       = {};
    char _pass[32]       = {};
    char _deviceName[32] = {};
    uint16_t _port       = 1883;

    MqttDisplayCallback _displayCb;
    MqttStateCallback   _stateCb;
    MqttSimpleCallback  _simpleCb;

    // Last known brightness, so a plain HA on/off toggle (no brightness field)
    // doesn't snap the panel back to full. Kept in sync by publishState().
    uint8_t _lastBrightness = 255;

    unsigned long _lastReconnect = 0;
};
