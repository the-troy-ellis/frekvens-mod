#include "web_server.h"
#include "config.h"
#include "storage.h"
#include "gif_decoder.h"
#include "games.h"
#include "zones.h"
#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

static AsyncWebServer  server(80);
static AsyncWebSocket  ws("/ws");

static WebUI* uiInstance  = nullptr;
static AppConfig* cfgPtr  = nullptr;

// --- Last-known-state caches ---
// lastBrightnessSeen mirrors the brightness of the most recent state broadcast so
// commands that omit "brightness" keep the current level instead of jumping to
// full (the MQTT path already behaved this way; the WS path defaulted to 255).
// lastFrame holds the most recent preview frame so a newly connected client gets
// the current picture immediately — frames are only broadcast on canvas changes
// now, not streamed continuously.
static uint8_t lastBrightnessSeen = 255;
// Sized for the maximum canvas (arrangements with gaps make the canvas larger
// than numPanels × 256 px — e.g. two panels flanking a speaker).
static uint8_t lastFrame[3 + CANVAS_MAX_W * CANVAS_MAX_H];
static size_t  lastFrameLen = 0;
// Cached copy of the most recent state broadcast, sent to newly connected WS
// clients so a freshly loaded page shows the device's actual mode/settings
// instead of defaults. Seeded by main.cpp broadcasting the boot state.
static char    lastStateJson[512];
static size_t  lastStateLen = 0;
// Same caching pattern for zone-mode state (on/off + each zone's assignment).
static char    lastZonesJson[512];
static size_t  lastZonesLen = 0;

// Clamp a JSON-supplied duration to a sane range: 0 would fire the display tick
// on every loop() pass, and out-of-range ints would wrap on uint16_t assignment.
static uint16_t clampMs(uint32_t ms) {
    return ms < 10 ? 10 : (ms > 60000 ? 60000 : (uint16_t)ms);
}

// --- JSON → DisplayState helper. Returns false if the message is malformed or
// invalid — the caller must IGNORE it. (A parse failure used to yield a default
// cmd=Off state, so any buggy client silently blanked the panel.) ---
static bool parseDisplayJson(const char* buf, size_t len, DisplayState& s) {
    // 768: a max-length (255-char) text message plus keys lands near 512's limit,
    // and a doc-full error would have blanked the panel via the old default-Off path.
    StaticJsonDocument<768> doc;
    if (deserializeJson(doc, buf, len)) return false;

    const char* cmd = doc["cmd"] | "off";
    int b = doc["brightness"] | (int)lastBrightnessSeen;   // omitted = keep current
    s.brightness = b < 0 ? 0 : (b > 255 ? 255 : b);
    s.power      = true;

    if (strcmp(cmd, "text") == 0) {
        s.cmd = DisplayCmd::Text;
        strncpy(s.text.text, doc["text"] | "", sizeof(s.text.text) - 1);
        s.text.brightness  = s.brightness;
        s.text.scrollMs    = clampMs(doc["scroll_ms"] | 80);
        s.text.scrollMode  = parseScrollMode(doc["scroll_mode"] | "horizontal_west");
        s.text.staticMode  = doc["static"] | false;
        s.text.once        = doc["once"]   | false;
        uint32_t gap = doc["gap_px"] | 0;
        s.text.gapPx = gap > 64 ? 64 : (uint8_t)gap;
        int ax = doc["anchor_x"] | -1, ay = doc["anchor_y"] | -1;
        s.text.anchorX = (ax >= 0 && ax < CANVAS_MAX_W) ? (int16_t)ax : -1;
        s.text.anchorY = (ay >= 0 && ay < CANVAS_MAX_H) ? (int16_t)ay : -1;
    } else if (strcmp(cmd, "clock") == 0) {
        s.cmd = DisplayCmd::Clock;
        s.clock.h24 = doc["h24"] | true;
        int ax = doc["anchor_x"] | -1, ay = doc["anchor_y"] | -1;
        s.clock.anchorX = (ax >= 0 && ax < CANVAS_MAX_W) ? (int16_t)ax : -1;
        s.clock.anchorY = (ay >= 0 && ay < CANVAS_MAX_H) ? (int16_t)ay : -1;
    } else if (strcmp(cmd, "weather") == 0) {
        s.cmd = DisplayCmd::Weather;
        strncpy(s.weather.condition, doc["condition"] | "auto", sizeof(s.weather.condition) - 1);
    } else if (strcmp(cmd, "game") == 0) {
        s.cmd = DisplayCmd::Game;
        strncpy(s.game.name, doc["name"] | "", sizeof(s.game.name) - 1);
        if (!s.game.name[0]) return false;
    } else if (strcmp(cmd, "image") == 0) {
        const char* name = doc["name"] | "";
        if (!validFileName(name)) return false;
        s.cmd = DisplayCmd::Image;
        strncpy(s.image.name, name, sizeof(s.image.name) - 1);
        s.image.brightness = s.brightness;
    } else if (strcmp(cmd, "animation") == 0) {
        const char* name = doc["name"] | "";
        if (!validFileName(name)) return false;
        s.cmd = DisplayCmd::Animation;
        strncpy(s.anim.name, name, sizeof(s.anim.name) - 1);
        s.anim.frameMs = clampMs(doc["frame_ms"] | 100);
        s.anim.loop    = doc["loop"]     | true;
    } else if (strcmp(cmd, "off") == 0) {
        s.cmd   = DisplayCmd::Off;
        s.power = false;
    } else {
        return false;   // unknown command — ignore rather than misinterpret as off
    }
    return true;
}

// --- WebSocket ---
static void onWsEvent(AsyncWebSocket*, AsyncWebSocketClient* client,
                      AwsEventType type, void* arg, uint8_t* data, size_t len) {
    if (type == WS_EVT_CONNECT) {
        // Sync the new client immediately: current state first (so the UI can
        // select the right tab/controls), then zone assignments, then the
        // current frame — broadcasts only happen on changes, so without this
        // a fresh page would show defaults and a blank preview until the next
        // change.
        if (lastStateLen) client->text(lastStateJson, lastStateLen);
        if (lastZonesLen) client->text(lastZonesJson, lastZonesLen);
        if (lastFrameLen) client->binary(lastFrame, lastFrameLen);
        return;
    }
    if (type != WS_EVT_DATA) return;
    AwsFrameInfo* info = (AwsFrameInfo*)arg;
    if (!info->final || info->index != 0 || info->len != len) return;
    if (info->opcode != WS_TEXT) return;
    if (!uiInstance) return;

    // Fast path for tiny high-rate messages — live-draw pixels and game
    // controller input — parsed with a small doc before the display-command
    // path. Only attempted for short payloads so normal commands aren't
    // parsed twice.
    if (len < 64) {
        StaticJsonDocument<96> pd;
        if (!deserializeJson(pd, (const char*)data, len)) {
            const char* c = pd["cmd"] | "";
            if (strcmp(c, "pixel") == 0 && uiInstance->_pixelCb) {
                int v = pd["v"] | 0;
                uiInstance->_pixelCb(pd["x"] | 0, pd["y"] | 0,
                                     (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)));
                return;
            }
            if (strcmp(c, "input") == 0 && uiInstance->_gameInputCb) {
                // {"cmd":"input","p":0,"k":"U","s":1,"z":2} — s omitted = press,
                // z omitted = the legacy whole-canvas game (-1); z present =
                // zone mode, target that zone's game.
                const char* k = pd["k"] | "";
                int p = pd["p"] | 0;
                int z = pd["z"] | -1;
                if (k[0]) uiInstance->_gameInputCb((uint8_t)(p < 0 ? 0 : p), k[0],
                                                   (pd["s"] | 1) != 0, (int8_t)z);
                return;
            }
        }
    }
    // Zone assignment ({"cmd":"zone","slot":0,"mode":"effect","sub":"life"}) —
    // small, low-rate messages, but not tiny enough to reliably fit the fast
    // path's 96-byte doc (mode+sub strings), so it gets its own parse.
    if (len < 160) {
        StaticJsonDocument<192> zd;
        if (!deserializeJson(zd, (const char*)data, len) &&
            strcmp(zd["cmd"] | "", "zone") == 0 && uiInstance->_zoneSetCb) {
            int slot = zd["slot"] | -1;
            if (slot >= 0 && slot < MAX_PANELS)
                uiInstance->_zoneSetCb((uint8_t)slot,
                                       (uint8_t)zoneModeByName(zd["mode"] | "off"),
                                       zd["sub"] | "");
            return;
        }
    }

    if (!uiInstance->_displayCb) return;
    DisplayState s;
    if (!parseDisplayJson((const char*)data, len, s)) return;
    uiInstance->_displayCb(s);
}

// --- HTTP helpers ---
static void sendJson(AsyncWebServerRequest* req, const char* json, int code = 200) {
    req->send(code, "application/json", json);
}

// Shared /api/images + /api/anims responder. Entries are {name, size} objects
// (the UI shows sizes); the MQTT file-list topics stay plain name arrays for
// backward compatibility with existing HA automations. Static buffers: 16
// name+size objects can exceed 1 KB, and the AsyncTCP task serves one request
// at a time, so statics are safe and keep the task stack small.
static void sendFileListJson(AsyncWebServerRequest* req, const char* key,
                             const char* dir, bool (*lister)(String*, uint8_t, uint8_t*)) {
    String names[16]; uint8_t found = 0;
    lister(names, 16, &found);
    static StaticJsonDocument<2048> doc;
    static char buf[2048];
    doc.clear();
    JsonArray arr = doc.createNestedArray(key);
    for (uint8_t i = 0; i < found; i++) {
        JsonObject o = arr.createNestedObject();
        o["name"] = names[i];
        String path = String(dir) + "/" + names[i];
        File f = LittleFS.open(path, "r");
        o["size"] = f ? f.size() : 0;
        if (f) f.close();
    }
    serializeJson(doc, buf, sizeof(buf));
    sendJson(req, buf);
}
static void sendOk(AsyncWebServerRequest* req) { sendJson(req, "{\"ok\":true}"); }
static void sendErr(AsyncWebServerRequest* req, const char* msg, int code = 400) {
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", msg);
    sendJson(req, buf, code);
}

// --- Upload temp state ---
// Shared by all three upload endpoints, so uploads must be serialized: two
// running at once (e.g. two browser tabs) would interleave writes into one
// file. First requester claims the slot; others get a 429 from their completion
// handler. The slot self-frees after 15 s without a chunk so an upload aborted
// mid-transfer (client vanished — its completion handler never runs) can't
// wedge uploads until reboot.
static File uploadFile;
static bool uploadIsGif = false;
static bool uploadOk    = false;   // false if open/write failed mid-upload
static char uploadName[64] = {};
static AsyncWebServerRequest* uploadOwner = nullptr;
static unsigned long uploadLastChunk = 0;

static bool uploadClaim(AsyncWebServerRequest* req) {
    if (uploadOwner == req) { uploadLastChunk = millis(); return true; }
    if (uploadOwner && millis() - uploadLastChunk < 15000) return false;
    if (uploadFile) uploadFile.close();   // abandoned upload's half-written file
    uploadOwner = req; uploadLastChunk = millis();
    return true;
}
static void uploadRelease(AsyncWebServerRequest* req) {
    if (uploadOwner == req) uploadOwner = nullptr;
}

void WebUI::begin(AppConfig* cfg) {
    _cfg       = cfg;
    cfgPtr     = cfg;
    uiInstance = this;
    lastBrightnessSeen = cfg->brightness;   // until the first state broadcast

    ws.onEvent(onWsEvent);
    server.addHandler(&ws);
    // no-cache so the browser re-fetches the UI after a filesystem update
    // (otherwise a stale cached app.js/index.html hides changes).
    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html").setCacheControl("no-cache");

    // --- Status ---
    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        StaticJsonDocument<384> doc;
        doc["ip"]       = WiFi.localIP().toString();
        doc["hostname"] = HOSTNAME;
        doc["panels"]   = cfgPtr ? cfgPtr->numPanels : 1;
        doc["rssi"]     = WiFi.RSSI();
        doc["heap"]     = ESP.getFreeHeap();
        doc["fs_used"]  = LittleFS.usedBytes();
        doc["fs_total"] = LittleFS.totalBytes();
        doc["uptime_s"] = millis() / 1000;
        doc["version"]  = __DATE__ " " __TIME__;   // firmware build timestamp
        char buf[384]; serializeJson(doc, buf, sizeof(buf));
        sendJson(req, buf);
    });

    // --- Game registry (the play page builds its picker from this) ---
    server.on("/api/games", HTTP_GET, [](AsyncWebServerRequest* req) {
        StaticJsonDocument<512> doc;
        JsonArray arr = doc.createNestedArray("games");
        for (uint8_t i = 0; i < gameCount(); i++) {
            JsonObject g = arr.createNestedObject();
            g["name"]    = gameInfo(i).name;
            g["title"]   = gameInfo(i).title;
            g["players"] = gameInfo(i).players;
        }
        char buf[512]; serializeJson(doc, buf, sizeof(buf));
        sendJson(req, buf);
    });

    // --- Zone info: current assignments + the pickable options for the admin
    // UI (curated effect names, the game registry, zone mode support). ---
    server.on("/api/zones", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!cfgPtr) { sendErr(req, "no config"); return; }
        StaticJsonDocument<768> doc;
        doc["on"] = cfgPtr->zoneModeOn;
        JsonArray zones = doc.createNestedArray("zones");
        for (uint8_t i = 0; i < MAX_PANELS; i++) {
            JsonObject z = zones.createNestedObject();
            z["slot"] = i;
            z["mode"] = zoneModeName((ZoneMode)cfgPtr->zoneSlots[i].mode);
            z["sub"]  = cfgPtr->zoneSlots[i].sub;
        }
        JsonArray effects = doc.createNestedArray("effects");
        for (uint8_t i = 0; i < zoneEffectCount(); i++) effects.add(zoneEffectName(i));
        JsonArray games = doc.createNestedArray("games");
        for (uint8_t i = 0; i < gameCount(); i++) games.add(gameInfo(i).name);
        char buf[768]; serializeJson(doc, buf, sizeof(buf));
        sendJson(req, buf);
    });

    // --- Config GET ---
    server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!cfgPtr) { sendErr(req, "no config"); return; }
        StaticJsonDocument<1024> doc;
        doc["mqtt_broker"]  = cfgPtr->mqttBroker;
        doc["mqtt_port"]    = cfgPtr->mqttPort;
        doc["mqtt_user"]    = cfgPtr->mqttUser;
        doc["mqtt_enabled"] = cfgPtr->mqttEnabled;
        doc["device_name"]  = cfgPtr->deviceName;
        doc["num_panels"]   = cfgPtr->numPanels;
        doc["brightness"]   = cfgPtr->brightness;
        doc["tz"]           = cfgPtr->tz;
        doc["gamma"]        = cfgPtr->gammaOn;
        doc["critter"]      = cfgPtr->critterOn;
        doc["zone_mode"]    = cfgPtr->zoneModeOn;
        // Arrangement: placements per chain position + derived canvas size, so
        // the UI can shape its preview/draw canvases and prefill the editor.
        JsonArray lay = doc.createNestedArray("layout");
        for (uint8_t i = 0; i < cfgPtr->numPanels; i++) {
            JsonObject p = lay.createNestedObject();
            p["x"]   = cfgPtr->layout[i].x;
            p["y"]   = cfgPtr->layout[i].y;
            p["rot"] = cfgPtr->layout[i].rot;
        }
        uint16_t cw, ch;
        layoutBounds(*cfgPtr, &cw, &ch);
        doc["canvas_w"] = cw;
        doc["canvas_h"] = ch;
        char buf[1024]; serializeJson(doc, buf, sizeof(buf));
        sendJson(req, buf);
    });

    // --- Config POST ---
    server.on("/api/config", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            if (!cfgPtr || !uiInstance) { sendErr(req, "not ready"); return; }
            StaticJsonDocument<1024> doc;
            if (deserializeJson(doc, data, len)) { sendErr(req, "invalid json"); return; }

            // Merge onto a local copy and let the command queue (processCommands)
            // be the only writer of the live cfg. Never strncpy directly into cfgPtr
            // from this (AsyncTCP) task — that races loop(). Every string read uses
            // the "| default" operator so a non-string JSON value yields "" instead
            // of nullptr (strncpy(dst, nullptr) is a crash).
            AppConfig c = *cfgPtr;
            if (doc.containsKey("mqtt_broker"))  strncpy(c.mqttBroker, doc["mqtt_broker"] | "", sizeof(c.mqttBroker) - 1);
            if (doc.containsKey("mqtt_port"))    c.mqttPort    = doc["mqtt_port"]   | 1883;
            if (doc.containsKey("mqtt_user"))    strncpy(c.mqttUser,   doc["mqtt_user"]  | "", sizeof(c.mqttUser) - 1);
            if (doc.containsKey("mqtt_pass"))    strncpy(c.mqttPass,   doc["mqtt_pass"]  | "", sizeof(c.mqttPass) - 1);
            if (doc.containsKey("mqtt_enabled")) c.mqttEnabled = doc["mqtt_enabled"] | false;
            if (doc.containsKey("device_name"))  strncpy(c.deviceName, doc["device_name"]| "", sizeof(c.deviceName) - 1);
            if (doc.containsKey("num_panels"))   c.numPanels   = doc["num_panels"]  | 1;
            if (doc.containsKey("brightness"))   c.brightness  = doc["brightness"]  | 255;
            if (doc.containsKey("tz"))           strncpy(c.tz, doc["tz"] | "", sizeof(c.tz) - 1);
            if (doc.containsKey("gamma"))        c.gammaOn     = doc["gamma"]       | false;
            if (doc.containsKey("critter"))      c.critterOn   = doc["critter"]     | true;
            if (doc.containsKey("zone_mode"))    c.zoneModeOn  = doc["zone_mode"]   | false;

            // Clamp panel count — the field is <input max=4> but the browser lets you
            // type anything, and an over-range count overflows the chain packet buffer.
            if (c.numPanels < 1)          c.numPanels = 1;
            if (c.numPanels > MAX_PANELS) c.numPanels = MAX_PANELS;

            // Arrangement (optional key — absent keeps the current placements).
            JsonArray lay = doc["layout"];
            if (!lay.isNull()) {
                uint8_t i = 0;
                for (JsonObject p : lay) {
                    if (i >= MAX_PANELS) break;
                    c.layout[i].x   = p["x"]   | 0;
                    c.layout[i].y   = p["y"]   | 0;
                    c.layout[i].rot = p["rot"] | 0;
                    i++;
                }
            }
            normalizeLayout(c);   // clamp coords/rot, shift bounding box to (0,0)

            if (uiInstance->_configCb) uiInstance->_configCb(c);
            sendOk(req);
        }
    );

    // --- Images list ---
    server.on("/api/images", HTTP_GET, [](AsyncWebServerRequest* req) {
        sendFileListJson(req, "images", IMAGE_DIR, Storage::listImages);
    });

    // --- Image upload (pre-converted 8-bit .raw from web UI client) ---
    server.on("/api/images/upload", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (uploadOwner != req) { sendErr(req, "another upload is in progress", 429); return; }
            uploadRelease(req);
            // Report the real result — a silent {"ok":true} on a failed write is what
            // made the old 31-char-filename bug invisible (open() fails on too-long
            // names; the client thought it succeeded).
            if (uploadOk) {
                sendOk(req);
                if (uiInstance->_filesChangedCb) uiInstance->_filesChangedCb();
            } else {
                sendErr(req, "write failed (invalid name, name too long, or storage full)", 500);
            }
        },
        [](AsyncWebServerRequest* req, const String& filename, size_t index,
           uint8_t* data, size_t len, bool final) {
            if (!uploadClaim(req)) return;   // busy — completion handler sends 429
            if (index == 0) {
                uploadOk = validFileName(filename.c_str());
                if (uploadOk) {
                    char path[80];
                    snprintf(path, sizeof(path), "%s/%s", IMAGE_DIR, filename.c_str());
                    uploadFile = LittleFS.open(path, "w");
                    uploadOk   = (bool)uploadFile;
                }
            }
            if (uploadOk && uploadFile && uploadFile.write(data, len) != len) uploadOk = false;
            if (final && uploadFile) uploadFile.close();
        }
    );

    // --- Image delete ---
    server.on("/api/images/delete", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            StaticJsonDocument<64> doc;
            if (deserializeJson(doc, data, len)) { sendErr(req, "bad json"); return; }
            const char* name = doc["name"] | "";
            if (!validFileName(name)) { sendErr(req, "invalid name"); return; }
            if (Storage::deleteImage(name)) {
                sendOk(req);
                if (uiInstance->_filesChangedCb) uiInstance->_filesChangedCb();
            } else {
                sendErr(req, "not found", 404);
            }
        }
    );

    // --- Animation list ---
    server.on("/api/anims", HTTP_GET, [](AsyncWebServerRequest* req) {
        sendFileListJson(req, "anims", ANIM_DIR, Storage::listAnims);
    });

    // --- GIF upload: accept GIF, decode in background after upload completes ---
    server.on("/api/anims/upload", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (uploadOwner != req) { sendErr(req, "another upload is in progress", 429); return; }
            uploadRelease(req);
            char tmpPath[80];
            snprintf(tmpPath, sizeof(tmpPath), "/tmp_%s", uploadName);
            // A non-GIF upload (or one that failed to write) still left a temp file
            // on flash — always remove it on the paths that won't decode it.
            if (strlen(uploadName) == 0 || !uploadIsGif || !uploadOk) {
                if (strlen(uploadName) > 0) LittleFS.remove(tmpPath);
                uploadName[0] = 0;
                if (!uploadOk) sendErr(req, "write failed (invalid name, name too long, or storage full)", 500);
                else           sendOk(req);
                return;
            }

            // After upload: decode the GIF that was stored to the temp path,
            // scaled to the current canvas (the arrangement's bounding box).
            char outPath[80];
            snprintf(outPath, sizeof(outPath), "%s/%s.anim", ANIM_DIR, uploadName);
            uint16_t cw = PANEL_WIDTH, ch = PANEL_HEIGHT;
            if (cfgPtr) layoutBounds(*cfgPtr, &cw, &ch);
            // ?invert=1 query param (from the Animation tab's Invert checkbox)
            bool invert = req->hasParam("invert") &&
                          req->getParam("invert")->value() == "1";
            // ?levels=N (2..16) posterises to N grey levels; absent/0 = full range.
            uint16_t levels = req->hasParam("levels")
                            ? (uint16_t)req->getParam("levels")->value().toInt() : 0;
            int frames = GifDecoder::decode(tmpPath, outPath,
                                            (uint8_t)cw, (uint8_t)ch, invert, levels);
            LittleFS.remove(tmpPath);
            if (frames > 0) {
                // Return the stored filename so app.js can play it immediately
                // without having to guess the .anim extension.
                char storedName[72];   // uploadName + ".anim"
                snprintf(storedName, sizeof(storedName), "%s.anim", uploadName);
                char resp[128];
                snprintf(resp, sizeof(resp),
                         "{\"ok\":true,\"frames\":%d,\"name\":\"%s\"}",
                         frames, storedName);
                req->send(200, "application/json", resp);
                if (uiInstance->_filesChangedCb) uiInstance->_filesChangedCb();
            } else {
                // decode() creates the output file before it parses any frames — a
                // failed decode would otherwise leave a partial/empty .anim behind,
                // visible in Saved Animations and playing as black.
                LittleFS.remove(outPath);
                sendErr(req, "gif decode failed");
            }
            uploadName[0] = 0;
        },
        [](AsyncWebServerRequest* req, const String& filename, size_t index,
           uint8_t* data, size_t len, bool final) {
            if (!uploadClaim(req)) return;   // busy — completion handler sends 429
            if (index == 0) {
                strncpy(uploadName, filename.c_str(), sizeof(uploadName) - 1);
                // Strip .gif suffix for the name
                char* dot = strrchr(uploadName, '.');
                if (dot) *dot = '\0';
                String lower = filename;
                lower.toLowerCase();               // accept .gif/.GIF/.Gif/...
                uploadIsGif = lower.endsWith(".gif");
                uploadOk = validFileName(uploadName);
                if (uploadOk) {
                    char tmpPath[80];
                    snprintf(tmpPath, sizeof(tmpPath), "/tmp_%s", uploadName);
                    uploadFile = LittleFS.open(tmpPath, "w");
                    uploadOk   = (bool)uploadFile;
                }
            }
            if (uploadOk && uploadFile && uploadFile.write(data, len) != len) uploadOk = false;
            if (final && uploadFile) uploadFile.close();
        }
    );

    // --- Raw .anim upload: the payload is already in the exact on-device binary
    // format (built client-side by the Draw tab's frame-by-frame animation feature),
    // so this just stores the bytes directly — no GIF decode needed. ---
    server.on("/api/anims/upload_raw", HTTP_POST,
        [](AsyncWebServerRequest* req) {
            if (uploadOwner != req) { sendErr(req, "another upload is in progress", 429); return; }
            uploadRelease(req);
            if (uploadOk) {
                sendOk(req);
                if (uiInstance->_filesChangedCb) uiInstance->_filesChangedCb();
            } else {
                sendErr(req, "write failed (invalid name, name too long, storage full, or not a valid .anim file)", 500);
            }
        },
        [](AsyncWebServerRequest* req, const String& filename, size_t index,
           uint8_t* data, size_t len, bool final) {
            if (!uploadClaim(req)) return;   // busy — completion handler sends 429
            if (index == 0) {
                // Reject anything not starting with the ANIM magic — this endpoint
                // skips the GIF decoder entirely, so a malformed file would otherwise
                // silently land on disk and only fail later, at playback time.
                if (len < 4 || memcmp(data, "ANIM", 4) != 0 || !validFileName(filename.c_str())) {
                    uploadOk = false;
                    return;
                }
                char path[80];
                snprintf(path, sizeof(path), "%s/%s", ANIM_DIR, filename.c_str());
                uploadFile = LittleFS.open(path, "w");
                uploadOk   = (bool)uploadFile;
            }
            if (uploadOk && uploadFile && uploadFile.write(data, len) != len) uploadOk = false;
            if (final && uploadFile) uploadFile.close();
        }
    );

    // --- Animation delete ---
    server.on("/api/anims/delete", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
            StaticJsonDocument<64> doc;
            if (deserializeJson(doc, data, len)) { sendErr(req, "bad json"); return; }
            // name already includes ".anim" (as returned by the list endpoint);
            // deleteAnim prepends the directory. Do not append ".anim" again.
            const char* name = doc["name"] | "";
            if (!validFileName(name)) { sendErr(req, "invalid name"); return; }
            if (Storage::deleteAnim(name)) {
                sendOk(req);
                if (uiInstance->_filesChangedCb) uiInstance->_filesChangedCb();
            } else {
                char path[80];
                snprintf(path, sizeof(path), "%s/%s", ANIM_DIR, name);
                if (LittleFS.exists(path)) {
                    // Exists but can't be removed → it's open, i.e. the anim
                    // currently playing (LittleFS refuses to remove open files).
                    // Hand it to the loop task, which stops playback (closing the
                    // handle) and finishes the delete — so respond ok now.
                    if (uiInstance->_deleteCb) uiInstance->_deleteCb(true, name);
                    sendOk(req);
                } else {
                    sendErr(req, "not found", 404);
                }
            }
        }
    );

    // --- Hardware test endpoints ---

    // GET /api/test/chain  — sends a white-fill frame + show to verify chain responds
    server.on("/api/test/chain", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!uiInstance || !uiInstance->_displayCb) { sendErr(req, "not ready"); return; }
        DisplayState s;
        s.cmd        = DisplayCmd::Text;
        s.power      = true;
        s.brightness = lastBrightnessSeen;   // keep the user's level, don't blast full
        strncpy(s.text.text, "TEST", sizeof(s.text.text) - 1);
        s.text.brightness = s.brightness;
        s.text.scrollMs   = 120;
        uiInstance->_displayCb(s);
        sendOk(req);
    });

    // GET /api/test/pixel?x=0&y=0&v=255  — set a single pixel for wiring debug
    server.on("/api/test/pixel", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!uiInstance || !uiInstance->_pixelCb) { sendErr(req, "not ready"); return; }
        int x = req->hasParam("x") ? req->getParam("x")->value().toInt() : 0;
        int y = req->hasParam("y") ? req->getParam("y")->value().toInt() : 0;
        int v = req->hasParam("v") ? req->getParam("v")->value().toInt() : 255;
        if (v < 0) v = 0; if (v > 255) v = 255;
        uiInstance->_pixelCb(x, y, (uint8_t)v);   // full 8-bit brightness
        sendOk(req);
    });

    // GET /api/test/sweep  — column sweep to verify all LEDs
    server.on("/api/test/sweep", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!uiInstance || !uiInstance->_displayCb) { sendErr(req, "not ready"); return; }
        DisplayState s;
        s.cmd          = DisplayCmd::Animation;
        s.power        = true;
        s.brightness   = lastBrightnessSeen;   // keep the user's level
        strncpy(s.anim.name, "__sweep__", sizeof(s.anim.name) - 1);
        s.anim.frameMs = 80;
        s.anim.loop    = true;
        uiInstance->_displayCb(s);
        sendOk(req);
    });

    server.onNotFound([](AsyncWebServerRequest* req) {
        req->send(404, "text/plain", "Not found");
    });

    server.begin();
}

void WebUI::broadcastState(const DisplayState& state) {
    // Runs (and updates the caches) even with no WS clients connected —
    // MQTT-driven changes must be reflected in lastBrightnessSeen, and a client
    // connecting later needs the cached state JSON.
    lastBrightnessSeen = state.brightness;
    StaticJsonDocument<512> doc;
    doc["type"]       = "state";
    doc["power"]      = state.power;
    doc["brightness"] = state.brightness;
    switch (state.cmd) {
        case DisplayCmd::Off:       doc["mode"] = "off";  break;
        case DisplayCmd::Text:      doc["mode"]        = "text";
                                    doc["text"]        = state.text.text;
                                    doc["scroll_mode"] = scrollModeToString(state.text.scrollMode);
                                    doc["scroll_ms"]   = state.text.scrollMs;
                                    doc["static"]      = state.text.staticMode;
                                    doc["once"]        = state.text.once;
                                    doc["gap_px"]      = state.text.gapPx;
                                    break;
        case DisplayCmd::Clock:     doc["mode"] = "clock";
                                    doc["h24"]  = state.clock.h24; break;
        case DisplayCmd::Weather:   doc["mode"]      = "weather";
                                    doc["condition"] = state.weather.condition; break;
        case DisplayCmd::Game:      doc["mode"] = "game";
                                    doc["name"] = state.game.name; break;
        case DisplayCmd::Image:     doc["mode"] = "image";
                                    doc["name"] = state.image.name; break;
        case DisplayCmd::Animation: doc["mode"]     = "animation";
                                    doc["name"]     = state.anim.name;
                                    doc["frame_ms"] = state.anim.frameMs;
                                    doc["loop"]     = state.anim.loop; break;
    }
    lastStateLen = serializeJson(doc, lastStateJson, sizeof(lastStateJson));
    if (ws.count()) ws.textAll(lastStateJson, lastStateLen);
}

void WebUI::broadcastFrame(const uint8_t* pixelData, uint8_t w, uint8_t h) {
    // Format: [0x01 type][w][h][w*h bytes of 8-bit pixels]. Always refresh the
    // lastFrame cache — even with no clients connected — so a client that
    // connects later gets the current picture immediately (frames are only
    // broadcast on canvas changes now, not streamed continuously).
    size_t pixBytes = (size_t)w * h;
    if (!pixelData) return;                         // canvas alloc failed — nothing to show
    if (pixBytes > sizeof(lastFrame) - 3) return;   // guard against an over-range request
    lastFrame[0] = 0x01; lastFrame[1] = w; lastFrame[2] = h;
    memcpy(lastFrame + 3, pixelData, pixBytes);
    lastFrameLen = 3 + pixBytes;
    if (ws.count() == 0) return;
    ws.binaryAll(lastFrame, lastFrameLen);
}

void WebUI::broadcastText(const char* json, size_t len) {
    if (ws.count()) ws.textAll(json, len);
}

void WebUI::broadcastZones(const AppConfig& cfg) {
    StaticJsonDocument<512> doc;
    doc["type"] = "zones";
    doc["on"]   = cfg.zoneModeOn;
    JsonArray zones = doc.createNestedArray("zones");
    for (uint8_t i = 0; i < MAX_PANELS; i++) {
        JsonObject z = zones.createNestedObject();
        z["mode"] = zoneModeName((ZoneMode)cfg.zoneSlots[i].mode);
        z["sub"]  = cfg.zoneSlots[i].sub;
    }
    lastZonesLen = serializeJson(doc, lastZonesJson, sizeof(lastZonesJson));
    if (ws.count()) ws.textAll(lastZonesJson, lastZonesLen);
}

void WebUI::service() {
    // Reap disconnected/stuck WebSocket clients. Slow or vanished clients (a
    // phone that slept, a dropped WiFi association) otherwise linger with their
    // queued frames pinned on the heap — the slow drain observed under heavy WS
    // churn. The library requires this to be called periodically.
    ws.cleanupClients();
}
