#include "mqtt_ha.h"
#include "config.h"
#include "storage.h"
#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

static WiFiClient   wifiClient;
static PubSubClient mqtt(wifiClient);
static MqttHA*      instance = nullptr;

static void mqttCallback(char* topic, byte* payload, unsigned int length) {
    if (instance) instance->onMessage(topic, payload, length);
}

void MqttHA::begin(const char* broker, uint16_t port,
                   const char* user, const char* pass,
                   const char* deviceName) {
    strncpy(_broker, broker, sizeof(_broker) - 1);
    strncpy(_user, user, sizeof(_user) - 1);
    strncpy(_pass, pass, sizeof(_pass) - 1);
    strncpy(_deviceName, deviceName, sizeof(_deviceName) - 1);
    _port = port;

    instance = this;
    // Drop any existing connection so a re-begin() (e.g. broker changed via the web
    // UI) reconnects to the new server instead of staying on the old one. Safe even
    // if never connected. Reset the reconnect timer so loop() retries immediately.
    mqtt.disconnect();
    mqtt.setServer(_broker, _port);
    mqtt.setCallback(mqttCallback);
    // 2048: the animation-select discovery payload carries the full options list
    // (16 near-max filenames ≈ 550 bytes) on top of the entity config + device
    // block — 1024 was close enough to fail silently on a full library.
    mqtt.setBufferSize(2048);
    _lastReconnect = 0;
}

void MqttHA::loop() {
    if (!mqtt.connected()) {
        unsigned long now = millis();
        if (now - _lastReconnect > 5000) {
            _lastReconnect = now;
            reconnect();
        }
    }
    mqtt.loop();
}

bool MqttHA::connected() const {
    return mqtt.connected();
}

void MqttHA::reconnect() {
    String clientId = String(HOSTNAME) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
    if (mqtt.connect(clientId.c_str(), _user, _pass,
                     MQTT_TOPIC_AVAILABILITY, 1, true, "offline")) {
        mqtt.publish(MQTT_TOPIC_AVAILABILITY, "online", true);
        mqtt.subscribe(MQTT_TOPIC_SET);
        mqtt.subscribe(MQTT_TOPIC_DISPLAY_SET);
        mqtt.subscribe(MQTT_TOPIC_TEXT_SET);
        mqtt.subscribe(MQTT_TOPIC_ANIM_SET);
        mqtt.subscribe(MQTT_TOPIC_SPEED_SET);
        mqtt.subscribe(MQTT_TOPIC_SCROLLMODE_SET);
        mqtt.subscribe(MQTT_TOPIC_WEATHER_SET);
        publishDiscovery();
        publishFileLists();   // fresh list on every (re)connect
    }
}

// Shared scratch for discovery/list payloads. Static (not stack): the anim
// select payload can reach ~1.5 KB and everything here runs on the loop task,
// one call at a time.
static StaticJsonDocument<1536> discDoc;
static char discPayload[1536];

static String uidBase() {
    return String(HOSTNAME) + "_" + String((uint32_t)ESP.getEfuseMac(), HEX);
}

// Common fields for the auxiliary entities (text/select/number): availability
// + the shared device block so HA groups them all under the one device.
static void addEntityCommon(JsonDocument& doc, const char* name,
                            const char* uidSuffix, const char* deviceName) {
    doc["name"]                  = name;
    doc["unique_id"]             = uidBase() + uidSuffix;
    doc["availability_topic"]    = MQTT_TOPIC_AVAILABILITY;
    doc["payload_available"]     = "online";
    doc["payload_not_available"] = "offline";
    JsonObject device = doc.createNestedObject("device");
    device["identifiers"][0] = uidBase();
    device["name"]           = deviceName;
    device["model"]          = "Frekvens Array";
    device["manufacturer"]   = "DIY";
}

static void publishDisc(const char* component, const char* objectSuffix) {
    char topic[128];
    snprintf(topic, sizeof(topic), "%s/%s/%s%s/config",
             MQTT_DISCOVERY_PREFIX, component, HOSTNAME, objectSuffix);
    serializeJson(discDoc, discPayload, sizeof(discPayload));
    mqtt.publish(topic, discPayload, true);
}

void MqttHA::publishDiscovery() {
    // --- Light entity (power + brightness) ---
    discDoc.clear();
    // HA convention for a device's single/main entity: set the entity's own name to
    // JSON null (not the device name string). With has_entity_name, HA then shows
    // just the device name alone instead of concatenating "<device> <entity>" —
    // which previously produced "Frekvens Array Frekvens Array" since both were the
    // same string.
    discDoc["name"]              = nullptr;
    discDoc["unique_id"]         = uidBase();
    discDoc["state_topic"]       = MQTT_TOPIC_STATE;
    discDoc["command_topic"]     = MQTT_TOPIC_SET;
    discDoc["brightness"]        = true;
    discDoc["brightness_scale"]  = 255;
    discDoc["schema"]            = "json";
    discDoc["availability_topic"]= MQTT_TOPIC_AVAILABILITY;
    discDoc["payload_available"] = "online";
    discDoc["payload_not_available"] = "offline";
    discDoc["retain"]            = true;
    JsonObject device = discDoc.createNestedObject("device");
    device["identifiers"][0] = uidBase();
    device["name"]           = _deviceName;
    device["model"]          = "Frekvens Array";
    device["manufacturer"]   = "DIY";
    publishDisc("light", "");

    // --- Text entity: send a message straight from an HA dashboard ---
    discDoc.clear();
    addEntityCommon(discDoc, "Message", "_text", _deviceName);
    discDoc["command_topic"] = MQTT_TOPIC_TEXT_SET;
    discDoc["state_topic"]   = MQTT_TOPIC_TEXT_STATE;
    discDoc["max"]           = 255;
    publishDisc("text", "_text");

    // --- Number entity: scroll speed (ms/px) ---
    discDoc.clear();
    addEntityCommon(discDoc, "Scroll speed", "_speed", _deviceName);
    discDoc["command_topic"] = MQTT_TOPIC_SPEED_SET;
    discDoc["state_topic"]   = MQTT_TOPIC_SPEED_STATE;
    discDoc["min"]  = 10;
    discDoc["max"]  = 1000;
    discDoc["step"] = 10;
    discDoc["unit_of_measurement"] = "ms";
    discDoc["mode"] = "box";
    publishDisc("number", "_speed");

    // --- Select entity: text scroll mode ---
    discDoc.clear();
    addEntityCommon(discDoc, "Scroll mode", "_scrollmode", _deviceName);
    discDoc["command_topic"] = MQTT_TOPIC_SCROLLMODE_SET;
    discDoc["state_topic"]   = MQTT_TOPIC_SCROLLMODE_STATE;
    {
        JsonArray opts = discDoc.createNestedArray("options");
        opts.add("horizontal_west");
        opts.add("horizontal_north");
        opts.add("horizontal_south");
        opts.add("vertical_north");
        opts.add("vertical_south");
    }
    publishDisc("select", "_scrollmode");

    // The animation select's options depend on the stored files — published by
    // publishFileLists() so it stays in sync with uploads/deletes.
}

static void publishNameList(const char* topic, const String* names, uint8_t found) {
    // Retained plain-array topics (frekvens/images, frekvens/anims) — kept as
    // simple name arrays for backward compatibility with existing automations.
    StaticJsonDocument<1024> doc;
    JsonArray arr = doc.to<JsonArray>();
    for (uint8_t i = 0; i < found; i++) arr.add(names[i]);
    char payload[1024];
    serializeJson(doc, payload, sizeof(payload));
    mqtt.publish(topic, payload, true);
}

void MqttHA::publishFileLists() {
    String names[16]; uint8_t found = 0;

    Storage::listImages(names, 16, &found);
    publishNameList(MQTT_TOPIC_IMAGES, names, found);

    Storage::listAnims(names, 16, &found);
    publishNameList(MQTT_TOPIC_ANIMS, names, found);

    // --- Select entity: animation picker, options = stored files + "(none)".
    // Discovery must be republished whenever the file set changes, because HA
    // select options live in the (retained) config payload.
    discDoc.clear();
    addEntityCommon(discDoc, "Animation", "_anim", _deviceName);
    discDoc["command_topic"] = MQTT_TOPIC_ANIM_SET;
    discDoc["state_topic"]   = MQTT_TOPIC_ANIM_STATE;
    JsonArray opts = discDoc.createNestedArray("options");
    opts.add("(none)");
    for (uint8_t i = 0; i < found; i++) opts.add(names[i]);
    publishDisc("select", "_anim");
}

void MqttHA::publishModeState(const char* text, const char* animName,
                              uint16_t scrollMs, const char* scrollMode) {
    mqtt.publish(MQTT_TOPIC_TEXT_STATE, text, true);
    mqtt.publish(MQTT_TOPIC_ANIM_STATE, animName, true);
    char b[12];
    snprintf(b, sizeof(b), "%u", scrollMs);
    mqtt.publish(MQTT_TOPIC_SPEED_STATE, b, true);
    mqtt.publish(MQTT_TOPIC_SCROLLMODE_STATE, scrollMode, true);
}

void MqttHA::publishState(bool on, uint8_t brightness) {
    _lastBrightness = brightness;   // track for on/off toggles that omit brightness
    StaticJsonDocument<64> doc;
    doc["state"]      = on ? "ON" : "OFF";
    doc["brightness"] = brightness;

    char payload[64];
    serializeJson(doc, payload, sizeof(payload));
    mqtt.publish(MQTT_TOPIC_STATE, payload, true);
}

void MqttHA::onMessage(const char* topic, const uint8_t* payload, unsigned int len) {
    // 512: covers the longest legit display/set command (a 255-char text message
    // plus JSON overhead ≈ 380 bytes). The old 256 silently truncated long text,
    // which then failed JSON parsing and dropped the command with no feedback.
    char buf[512] = {};
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, payload, len);

    if (strcmp(topic, MQTT_TOPIC_SET) == 0) {
        StaticJsonDocument<128> doc;
        if (!deserializeJson(doc, buf)) {
            bool on = true;
            // Default to the last known level so a bare {"state":"ON"} toggle
            // keeps the user's brightness instead of jumping to full.
            uint8_t brightness = _lastBrightness;
            // "| default" reads are type-safe: a non-string "state" (e.g. {"state":1})
            // yields "OFF" here rather than nullptr, avoiding a strcmp(nullptr) crash
            // that any broker client could otherwise trigger.
            if (doc.containsKey("state"))      on         = strcmp(doc["state"] | "OFF", "ON") == 0;
            if (doc.containsKey("brightness")) {
                // Clamp before the uint8_t assignment — e.g. 300 would wrap to 44.
                int b = doc["brightness"] | (int)_lastBrightness;
                brightness = b < 0 ? 0 : (b > 255 ? 255 : b);
            }
            if (_stateCb) _stateCb(on, brightness);
        }
    } else if (strcmp(topic, MQTT_TOPIC_DISPLAY_SET) == 0) {
        if (_displayCb) _displayCb(buf);
    } else if (strcmp(topic, MQTT_TOPIC_TEXT_SET) == 0) {
        if (_simpleCb) _simpleCb("text", buf);
    } else if (strcmp(topic, MQTT_TOPIC_ANIM_SET) == 0) {
        if (_simpleCb) _simpleCb("anim", buf);
    } else if (strcmp(topic, MQTT_TOPIC_SPEED_SET) == 0) {
        if (_simpleCb) _simpleCb("speed", buf);
    } else if (strcmp(topic, MQTT_TOPIC_SCROLLMODE_SET) == 0) {
        if (_simpleCb) _simpleCb("scrollmode", buf);
    } else if (strcmp(topic, MQTT_TOPIC_WEATHER_SET) == 0) {
        if (_simpleCb) _simpleCb("weather", buf);
    }
}
