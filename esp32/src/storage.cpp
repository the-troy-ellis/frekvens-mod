#include "storage.h"
#include "config.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

bool Storage::begin() {
    if (!LittleFS.begin(true)) return false;
    LittleFS.mkdir(IMAGE_DIR);
    LittleFS.mkdir(ANIM_DIR);

    // Sweep orphaned GIF-upload temp files (/tmp_<name>): normal cleanup happens
    // in the upload's completion handler, so an upload interrupted by a reset
    // leaves its temp file behind — and they'd accumulate on flash forever.
    // Collect first, remove after: deleting while iterating the dir handle is
    // unreliable on LittleFS.
    String stale[8]; uint8_t nStale = 0;
    File root = LittleFS.open("/");
    if (root && root.isDirectory()) {
        File e = root.openNextFile();
        while (e && nStale < 8) {
            const char* n = e.name();
            const char* slash = strrchr(n, '/');
            const char* base = slash ? slash + 1 : n;
            if (!e.isDirectory() && strncmp(base, "tmp_", 4) == 0)
                stale[nStale++] = String("/") + base;
            e = root.openNextFile();
        }
    }
    for (uint8_t i = 0; i < nStale; i++) LittleFS.remove(stale[i]);
    return true;
}

bool Storage::loadConfig(AppConfig& cfg) {
    File f = LittleFS.open(CONFIG_FILE, "r");
    if (!f) return false;

    // 1024: layout + zones arrays together push past 768's pool limit.
    StaticJsonDocument<1024> doc;
    if (deserializeJson(doc, f)) { f.close(); return false; }
    f.close();

    strncpy(cfg.mqttBroker,  doc["mqtt_broker"] | "", sizeof(cfg.mqttBroker) - 1);
    cfg.mqttPort    = doc["mqtt_port"]    | 1883;
    strncpy(cfg.mqttUser,    doc["mqtt_user"]   | "", sizeof(cfg.mqttUser) - 1);
    strncpy(cfg.mqttPass,    doc["mqtt_pass"]   | "", sizeof(cfg.mqttPass) - 1);
    strncpy(cfg.deviceName,  doc["device_name"] | "Frekvens Array", sizeof(cfg.deviceName) - 1);
    cfg.numPanels   = doc["num_panels"]   | 1;
    // Clamp to a sane range: a corrupt/hand-edited config with numPanels > MAX_PANELS
    // would overflow the chain packet buffer on the very first flush after boot.
    if (cfg.numPanels < 1)          cfg.numPanels = 1;
    if (cfg.numPanels > MAX_PANELS) cfg.numPanels = MAX_PANELS;
    cfg.brightness  = doc["brightness"]   | 255;   // 8-bit scale (full)
    cfg.mqttEnabled = doc["mqtt_enabled"] | false;
    strncpy(cfg.tz, doc["tz"] | "", sizeof(cfg.tz) - 1);
    cfg.gammaOn     = doc["gamma"]        | false;
    cfg.critterOn   = doc["critter"]      | true;

    // Panel arrangement. Absent (pre-arrangement config) = keep the AppConfig
    // constructor's default vertical stack — full backward compatibility.
    JsonArray lay = doc["layout"];
    if (!lay.isNull()) {
        uint8_t i = 0;
        for (JsonObject p : lay) {
            if (i >= MAX_PANELS) break;
            cfg.layout[i].x   = p["x"]   | 0;
            cfg.layout[i].y   = p["y"]   | 0;
            cfg.layout[i].rot = p["rot"] | 0;
            i++;
        }
    }
    normalizeLayout(cfg);

    // Zone mode. Absent (pre-zones config) = zoneModeOn false, all slots Off
    // (the AppConfig constructor's defaults) — full backward compatibility.
    cfg.zoneModeOn = doc["zone_mode"] | false;
    JsonArray zones = doc["zones"];
    if (!zones.isNull()) {
        uint8_t i = 0;
        for (JsonObject z : zones) {
            if (i >= MAX_PANELS) break;
            cfg.zoneSlots[i].mode = z["mode"] | 0;
            strncpy(cfg.zoneSlots[i].sub, z["sub"] | "", sizeof(cfg.zoneSlots[i].sub) - 1);
            i++;
        }
    }
    normalizeZones(cfg);
    return true;
}

bool Storage::saveConfig(const AppConfig& cfg) {
    File f = LittleFS.open(CONFIG_FILE, "w");
    if (!f) return false;

    StaticJsonDocument<1024> doc;
    doc["mqtt_broker"]  = cfg.mqttBroker;
    doc["mqtt_port"]    = cfg.mqttPort;
    doc["mqtt_user"]    = cfg.mqttUser;
    doc["mqtt_pass"]    = cfg.mqttPass;
    doc["device_name"]  = cfg.deviceName;
    doc["num_panels"]   = cfg.numPanels;
    doc["brightness"]   = cfg.brightness;
    doc["mqtt_enabled"] = cfg.mqttEnabled;
    doc["tz"]           = cfg.tz;
    doc["gamma"]        = cfg.gammaOn;
    doc["critter"]      = cfg.critterOn;
    JsonArray lay = doc.createNestedArray("layout");
    for (uint8_t i = 0; i < cfg.numPanels; i++) {
        JsonObject p = lay.createNestedObject();
        p["x"]   = cfg.layout[i].x;
        p["y"]   = cfg.layout[i].y;
        p["rot"] = cfg.layout[i].rot;
    }
    doc["zone_mode"] = cfg.zoneModeOn;
    JsonArray zones = doc.createNestedArray("zones");
    for (uint8_t i = 0; i < MAX_PANELS; i++) {
        JsonObject z = zones.createNestedObject();
        z["mode"] = cfg.zoneSlots[i].mode;
        z["sub"]  = cfg.zoneSlots[i].sub;
    }

    serializeJson(doc, f);
    f.close();
    return true;
}

static bool listDir(const char* dir, String* names, uint8_t maxCount, uint8_t* found) {
    *found = 0;
    File d = LittleFS.open(dir);
    if (!d || !d.isDirectory()) return false;
    File entry = d.openNextFile();
    while (entry && *found < maxCount) {
        if (!entry.isDirectory()) {
            // Normalize to basename — File::name() may return a full path on some
            // cores, and load/delete prepend the directory themselves.
            const char* n = entry.name();
            const char* slash = strrchr(n, '/');
            const char* base = slash ? slash + 1 : n;
            // Reserved "__name__" convention (matches the built-in __sweep__ display
            // mode): internal/ephemeral files — e.g. the Draw tab's live panel
            // preview — that should never appear in any user-facing list (web UI,
            // HA dropdown sync) even though they're real files on flash.
            bool hidden = base[0] == '_' && base[1] == '_';
            if (!hidden) names[(*found)++] = String(base);
        }
        entry = d.openNextFile();
    }
    return true;
}

bool Storage::listImages(String* names, uint8_t maxCount, uint8_t* found) {
    return listDir(IMAGE_DIR, names, maxCount, found);
}

bool Storage::listAnims(String* names, uint8_t maxCount, uint8_t* found) {
    return listDir(ANIM_DIR, names, maxCount, found);
}

static bool saveFile(const char* dir, const char* name, const uint8_t* data, size_t len) {
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    File f = LittleFS.open(path, "w");
    if (!f) return false;
    f.write(data, len);
    f.close();
    return true;
}

static bool loadFile(const char* dir, const char* name, uint8_t* data, size_t maxLen, size_t* len) {
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    *len = f.read(data, maxLen);
    f.close();
    return true;
}

static bool deleteFile(const char* dir, const char* name) {
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    return LittleFS.remove(path);
}

bool Storage::saveImage(const char* n, const uint8_t* d, size_t l)               { return saveFile(IMAGE_DIR, n, d, l); }
bool Storage::loadImage(const char* n, uint8_t* d, size_t m, size_t* l)          { return loadFile(IMAGE_DIR, n, d, m, l); }
bool Storage::deleteImage(const char* n)                                           { return deleteFile(IMAGE_DIR, n); }
bool Storage::saveAnim(const char* n, const uint8_t* d, size_t l)                { return saveFile(ANIM_DIR, n, d, l); }
bool Storage::loadAnim(const char* n, uint8_t* d, size_t m, size_t* l)           { return loadFile(ANIM_DIR, n, d, m, l); }
bool Storage::deleteAnim(const char* n)                                            { return deleteFile(ANIM_DIR, n); }
