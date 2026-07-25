#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <time.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "config.h"
#include "storage.h"
#include "renderer.h"
#include "panel_chain.h"
#include "mqtt_ha.h"
#include "web_server.h"
#include "gif_decoder.h"
#include "effects.h"
#include "games.h"
#include "zones.h"

// --- Globals ---
static AppConfig    cfg;
static Renderer*    renderer   = nullptr;
static PanelChain*  chain      = nullptr;
static MqttHA       mqtt;
static WebUI        webUI;
static DisplayState dispState;

static unsigned long lastFlush     = 0;
static unsigned long lastKeepalive = 0;
static unsigned long lastWsService = 0;
static unsigned long lastScroll    = 0;
static unsigned long lastAnimFrame = 0;
static unsigned long lastBrightSend = 0;
static unsigned long lastGameNet   = 0;
static int           sentBrightness = -1;   // -1 = never sent → forces first send

// Set whenever the canvas content changes (mode change, scroll tick, anim frame,
// pixel command). loop() only pushes to the chain / WS preview when this is set
// (plus a 1 Hz keepalive) — static content used to re-send identical 1 KB
// packets 20×/s forever.
static bool   canvasDirty     = true;

static char   scrollText[256] = {};
static int    scrollX = 0, scrollY = 0;   // current draw position (screen px, X right+/Y down+)
static bool   scrollDirty     = true;

// Animation playback — using .anim format with header
static File      animFile;
static uint16_t  animNumFrames  = 0;
static uint16_t  animFrameIdx   = 0;
static uint16_t  animFrameBytes = 0;
static uint8_t   animW          = 0;   // frame dims baked into the .anim header —
static uint8_t   animH          = 0;   //   NOT the current cfg (which may differ)
static uint16_t* animDelays     = nullptr;  // per-frame delay table, cached at load
                                            // (saves a file seek per frame)
// Frame read buffer — allocated once per anim load (header validation caps
// dims at CANVAS_MAX_*), so playback still does no per-frame heap churn.
static uint8_t*  animBuf        = nullptr;

// The active built-in effect (engine + all state live in effects.cpp).
static Effect effect = Effect::None;

// Weather ambience: the scene currently rendering, and the live condition Home
// Assistant keeps current on frekvens/weather/set. A weather command with
// condition "auto" resolves to liveWeather, so the panel tracks the real sky.
static Weather weatherScene = Weather::Clouds;
static char    liveWeather[24] = "cloudy";
static unsigned long lastWeather = 0;

// Bumped every time content repaints the canvas (mode change, clock redraw,
// static text render). Pip's save-under restore checks it: a changed epoch
// means the content already painted over him, so the stale restore is skipped.
static uint32_t contentEpoch = 0;

// Largest contiguous panel-covered rectangle on the canvas (canvas px),
// computed from the layout. Centered content (clock, static text, boot icons)
// places itself here so it can't land in a gap — e.g. the dead space where a
// speaker module sits between two panels. For any contiguous arrangement this
// is simply the whole canvas.
static uint16_t crX = 0, crY = 0, crW = PANEL_WIDTH, crH = PANEL_HEIGHT;

// Gamma-2.2 LUT for perceptual brightness (built once in setup; enabled per
// cfg.gammaOn via Renderer::setGamma — applied at serialisation, not on the canvas).
static uint8_t gammaLut[256];

// Deferred config save (debounce). A brightness-slider drag fires many display
// commands; instead of hammering LittleFS, we mark it dirty and save once the
// value settles.
static bool          configDirty   = false;
static unsigned long configDirtyAt = 0;

// --- Cross-task command queue ---
// Web-server callbacks run in the AsyncTCP task and MUST NOT touch renderer /
// chain / cfg / dispState / MQTT directly (that races loop(), and delete-ing the
// renderer under a running flush() crashes). They enqueue a Command instead;
// loop() drains the queue and does all the work in one task.
enum class CmdType : uint8_t { Display, Config, Pixel, FilesChanged, DeleteFile, ZoneSet };
struct Command {
    CmdType      type;
    DisplayState display;      // Display
    AppConfig    config;       // Config
    int16_t      px, py;       // Pixel
    uint8_t      pv;
    bool         isAnim;       // DeleteFile
    char         fileName[64]; // DeleteFile
    uint8_t      zSlot;        // ZoneSet
    uint8_t      zMode;        // ZoneSet (raw ZoneMode value)
    char         zSub[24];     // ZoneSet
};
static QueueHandle_t cmdQueue = nullptr;

// Controller events from the play page. Separate small queue (not Command —
// that struct is ~650 bytes and inputs arrive at button-mash rates); drained
// every loop() pass, so input-to-game latency is ~1 ms plus WiFi.
// z: -1 = the legacy whole-canvas game, 0..3 = target that zone's game.
struct GameInputEvt { uint8_t p; char k; uint8_t s; int8_t z; };
static QueueHandle_t gameInQueue = nullptr;

// --- Forward declarations ---
static void applyDisplayState(const DisplayState& s);
static void tickDisplay();
static void processCommands();
static void setupWifi();
static void setupOTA();

// MQTT is usable only when enabled AND a broker is set. Servicing mqtt.loop() with
// an empty broker makes PubSubClient retry a dead connection every 5 s, blocking and
// stalling animations — so this guards both begin() and the loop() service call.
static bool mqttConfigured() {
    return cfg.mqttEnabled && strlen(cfg.mqttBroker) > 0;
}

// Publish everything HA tracks about the current display state: the light's
// power/brightness plus the native entities (message text, animation select,
// scroll speed, scroll mode). One choke point so no state-change path forgets one.
static void publishHaStates() {
    if (!cfg.mqttEnabled) return;
    mqtt.publishState(dispState.power, dispState.brightness);
    const char* animHa = "(none)";
    if (dispState.cmd == DisplayCmd::Animation &&
        strncmp(dispState.anim.name, "__", 2) != 0)   // effects/tests aren't select options
        animHa = dispState.anim.name;
    mqtt.publishModeState(dispState.text.text, animHa, dispState.text.scrollMs,
                          scrollModeToString(dispState.text.scrollMode));
}

// (Re)apply the SNTP + timezone config for clock mode. cfg.tz is a POSIX TZ
// string ("PST8PDT,M3.2.0,M11.1.0"); empty = UTC.
static void applyTimeConfig() {
    configTzTime(cfg.tz[0] ? cfg.tz : "GMT0", "pool.ntp.org", "time.nist.gov");
}

static void applyRendererOptions() {
    renderer->setGamma(cfg.gammaOn ? gammaLut : nullptr);
}

// "Calm" = static-ish content Pip may wander in front of. Never during
// animations/effects/scrolling (busy), never while the panel is off, and never
// over hidden __ images — that's the Draw tab's live canvas being edited.
static bool displayCalm() {
    if (!dispState.power) return false;
    switch (dispState.cmd) {
        case DisplayCmd::Clock: return true;
        case DisplayCmd::Image: return strncmp(dispState.image.name, "__", 2) != 0;
        case DisplayCmd::Text:  return dispState.text.staticMode;
        default:                return false;
    }
}

// Largest rectangle fully covered by panels, via the classic largest-rectangle-
// in-a-binary-matrix algorithm (per-row histogram + stack, O(W*H)). Fully
// general: works for pixel-offset placements, overlaps, any shape.
static void computeCenterRect() {
    int W = renderer->width(), H = renderer->height();
    crX = 0; crY = 0; crW = W; crH = H;   // fallback: whole canvas

    uint8_t*  mask = (uint8_t*)calloc((size_t)W * H, 1);
    uint16_t* hgt  = (uint16_t*)calloc(W, sizeof(uint16_t));
    if (!mask || !hgt) { free(mask); free(hgt); return; }

    for (uint8_t k = 0; k < cfg.numPanels; k++) {
        for (int y = 0; y < PANEL_HEIGHT; y++)
            for (int x = 0; x < PANEL_WIDTH; x++) {
                int cx = cfg.layout[k].x + x, cy = cfg.layout[k].y + y;
                if (cx >= 0 && cx < W && cy >= 0 && cy < H) mask[(size_t)cy * W + cx] = 1;
            }
    }

    int32_t best = 0;
    int16_t stack[CANVAS_MAX_W + 2];
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++)
            hgt[x] = mask[(size_t)y * W + x] ? hgt[x] + 1 : 0;
        int sp = 0;
        stack[0] = -1;
        for (int x = 0; x <= W; x++) {
            uint16_t cur = (x < W) ? hgt[x] : 0;
            while (stack[sp] != -1 && hgt[stack[sp]] >= cur) {
                int hVal = hgt[stack[sp--]];
                int left = stack[sp] + 1;
                int wRect = x - left;
                if ((int32_t)hVal * wRect > best) {
                    best = (int32_t)hVal * wRect;
                    crX = left; crY = y - hVal + 1;
                    crW = wRect; crH = hVal;
                }
            }
            stack[++sp] = x;
        }
    }
    free(mask); free(hgt);
    if (best == 0) { crX = 0; crY = 0; crW = W; crH = H; }
}

// (Re)size everything derived from the layout: effect scratch buffers scale
// with the canvas, and the centered-content rectangle depends on panel
// positions. Call after every renderer (re)creation, before content renders.
static void applyLayoutDerived() {
    effectsLayoutChanged(*renderer);
    computeCenterRect();
}

// (Re)start every configured zone from cfg.zoneSlots. Safe to call whether or
// not zone mode is currently on; harmless when it's off (zones just sit idle,
// composited nowhere). Call after boot and after any Config change (numPanels,
// or the zone_mode toggle itself, may have changed).
static void applyZonesFromConfig() {
    for (uint8_t i = 0; i < MAX_PANELS; i++)
        zoneApply(i, (ZoneMode)cfg.zoneSlots[i].mode, cfg.zoneSlots[i].sub);
    canvasDirty = true;   // push the freshly (re)rendered zones on the next flush
}

// --- Setup ---
void setup() {
    Serial.begin(115200);

    if (!Storage::begin()) Serial.println("LittleFS mount failed");
    Storage::loadConfig(cfg);

    // 12 deep (~7 KB): a brightness-slider drag can burst faster than one loop()
    // pass drains, and xQueueSend with a 0 timeout silently drops on full — a
    // dropped *final* slider value would leave stale brightness.
    cmdQueue    = xQueueCreate(12, sizeof(Command));
    gameInQueue = xQueueCreate(16, sizeof(GameInputEvt));

    // Gamma-2.2 LUT (256 entries, built once). Enabled per config.
    for (int i = 0; i < 256; i++)
        gammaLut[i] = (uint8_t)lroundf(255.0f * powf(i / 255.0f, 2.2f));

    uint16_t cw, ch;
    layoutBounds(cfg, &cw, &ch);
    renderer = new Renderer(cw, ch, cfg.layout, cfg.numPanels);
    chain    = new PanelChain(cfg.numPanels);
    chain->begin();
    applyRendererOptions();
    applyLayoutDerived();
    zonesInit(&cfg);
    applyZonesFromConfig();

    // Apply the saved brightness so a reboot restores the user's level
    // instead of coming up at full (the loop pushes it to the panels).
    dispState.brightness = cfg.brightness;

    setupWifi();
    setupOTA();
    MDNS.addService("http", "tcp", 80);   // OTA's begin() already ran MDNS.begin(HOSTNAME)
    applyTimeConfig();                    // SNTP + timezone for clock mode

    webUI.begin(&cfg);
    webUI.onDisplayChange([](const DisplayState& s) {
        Command c{}; c.type = CmdType::Display; c.display = s;
        xQueueSend(cmdQueue, &c, 0);
    });
    webUI.onSetPixel([](int x, int y, uint8_t v) {
        Command c{}; c.type = CmdType::Pixel; c.px = x; c.py = y; c.pv = v;
        xQueueSend(cmdQueue, &c, 0);
    });
    webUI.onConfigSave([](const AppConfig& newCfg) {
        Command c{}; c.type = CmdType::Config; c.config = newCfg;
        xQueueSend(cmdQueue, &c, 0);
    });
    webUI.onFilesChanged([]() {
        // Just a signal — processCommands re-publishes the MQTT file lists, which
        // must happen on the loop task (PubSubClient isn't safe to call from the
        // AsyncTCP task this callback actually runs in).
        Command c{}; c.type = CmdType::FilesChanged;
        xQueueSend(cmdQueue, &c, 0);
    });
    webUI.onDeleteFile([](bool isAnim, const char* name) {
        // Delete of the currently-playing file — LittleFS can't remove an open
        // file, so the loop task stops playback (closing the handle) and retries.
        Command c{}; c.type = CmdType::DeleteFile; c.isAnim = isAnim;
        strncpy(c.fileName, name, sizeof(c.fileName) - 1);
        xQueueSend(cmdQueue, &c, 0);
    });
    webUI.onGameInput([](uint8_t p, char k, bool pressed, int8_t zoneSlot) {
        GameInputEvt e{ p, k, (uint8_t)pressed, zoneSlot };
        xQueueSend(gameInQueue, &e, 0);   // full queue = mash events dropped, fine
    });
    webUI.onZoneSet([](uint8_t slot, uint8_t mode, const char* sub) {
        Command c{}; c.type = CmdType::ZoneSet; c.zSlot = slot; c.zMode = mode;
        strncpy(c.zSub, sub, sizeof(c.zSub) - 1);
        xQueueSend(cmdQueue, &c, 0);
    });

    // Register MQTT callbacks unconditionally — they only enqueue commands, so
    // they're harmless when MQTT is disabled. This lets MQTT be turned on later
    // from the web UI without a reboot (the old code only wired these up at boot
    // when MQTT was already enabled).
    mqtt.onStateSet([](bool on, uint8_t brightness) {
        // Runs in loop() task via mqtt.loop(); reading dispState is safe here.
        Command c{}; c.type = CmdType::Display; c.display = dispState;
        c.display.power = on; c.display.brightness = brightness;
        xQueueSend(cmdQueue, &c, 0);
    });
    mqtt.onDisplaySet([](const char* json) {
        // 768: a max-length (255-char) text message plus keys lands near 512's limit.
        StaticJsonDocument<768> doc;
        if (deserializeJson(doc, json)) return;
        DisplayState s = dispState;
        // Effective brightness is the top-level DisplayState field. It was
        // previously written only to TextParams/ImageParams.brightness — which
        // nothing reads — so a brightness in display/set was silently ignored.
        int b = doc["brightness"] | (int)dispState.brightness;
        s.brightness = b < 0 ? 0 : (b > 255 ? 255 : b);
        // Durations are clamped: 0 would fire the display tick every loop() pass,
        // and out-of-range ints would wrap on the uint16_t assignment.
        const char* mode = doc["mode"] | "off";
        if (strcmp(mode, "text") == 0) {
            s.cmd = DisplayCmd::Text;
            strncpy(s.text.text, doc["text"] | "", sizeof(s.text.text) - 1);
            uint32_t ms = doc["scroll_ms"] | 80;
            s.text.scrollMs   = ms < 10 ? 10 : (ms > 60000 ? 60000 : ms);
            s.text.scrollMode = parseScrollMode(doc["scroll_mode"] | "horizontal_west");
            s.text.staticMode = doc["static"] | false;
            s.text.once       = doc["once"]   | false;
            uint32_t gap = doc["gap_px"] | 0;
            s.text.gapPx = gap > 64 ? 64 : (uint8_t)gap;
            int ax = doc["anchor_x"] | -1, ay = doc["anchor_y"] | -1;
            s.text.anchorX = (ax >= 0 && ax < CANVAS_MAX_W) ? (int16_t)ax : -1;
            s.text.anchorY = (ay >= 0 && ay < CANVAS_MAX_H) ? (int16_t)ay : -1;
        } else if (strcmp(mode, "clock") == 0) {
            s.cmd = DisplayCmd::Clock;
            s.clock.h24 = doc["h24"] | true;
            int ax = doc["anchor_x"] | -1, ay = doc["anchor_y"] | -1;
            s.clock.anchorX = (ax >= 0 && ax < CANVAS_MAX_W) ? (int16_t)ax : -1;
            s.clock.anchorY = (ay >= 0 && ay < CANVAS_MAX_H) ? (int16_t)ay : -1;
        } else if (strcmp(mode, "weather") == 0) {
            s.cmd = DisplayCmd::Weather;
            strncpy(s.weather.condition, doc["condition"] | "auto", sizeof(s.weather.condition) - 1);
        } else if (strcmp(mode, "game") == 0) {
            s.cmd = DisplayCmd::Game;
            strncpy(s.game.name, doc["name"] | "", sizeof(s.game.name) - 1);
            if (!s.game.name[0]) return;
        } else if (strcmp(mode, "image") == 0) {
            const char* name = doc["name"] | "";
            if (!validFileName(name)) return;   // reject path escapes / empty names
            s.cmd = DisplayCmd::Image;
            strncpy(s.image.name, name, sizeof(s.image.name) - 1);
        } else if (strcmp(mode, "animation") == 0) {
            const char* name = doc["name"] | "";
            if (!validFileName(name)) return;
            s.cmd = DisplayCmd::Animation;
            strncpy(s.anim.name, name, sizeof(s.anim.name) - 1);
            uint32_t ms = doc["frame_ms"] | 100;
            s.anim.frameMs = ms < 10 ? 10 : (ms > 60000 ? 60000 : ms);
            s.anim.loop    = doc["loop"]     | true;
        } else {
            s.cmd   = DisplayCmd::Off;
            s.power = false;   // match the WS off command's semantics
        }
        Command c{}; c.type = CmdType::Display; c.display = s;
        xQueueSend(cmdQueue, &c, 0);
    });

    // Native HA entity commands (text / anim select / speed / scroll mode).
    // Runs in the loop task via mqtt.loop(), so reading dispState is safe here.
    mqtt.onSimpleSet([](const char* kind, const char* value) {
        DisplayState s = dispState;
        if (strcmp(kind, "text") == 0) {
            s.cmd = DisplayCmd::Text;
            strncpy(s.text.text, value, sizeof(s.text.text) - 1);
            s.text.text[sizeof(s.text.text) - 1] = 0;
        } else if (strcmp(kind, "anim") == 0) {
            if (strcmp(value, "(none)") == 0) {
                s.cmd = DisplayCmd::Off;
                s.power = false;
            } else if (validFileName(value)) {
                s.cmd = DisplayCmd::Animation;
                strncpy(s.anim.name, value, sizeof(s.anim.name) - 1);
                if (s.anim.frameMs < 10) s.anim.frameMs = 100;
                s.anim.loop = true;
            } else return;
        } else if (strcmp(kind, "speed") == 0) {
            long v = atol(value);
            s.text.scrollMs = v < 10 ? 10 : (v > 60000 ? 60000 : (uint16_t)v);
        } else if (strcmp(kind, "scrollmode") == 0) {
            s.text.scrollMode = parseScrollMode(value);
        } else if (strcmp(kind, "weather") == 0) {
            // Home Assistant keeps the live sky condition current here. Cache it;
            // only re-render if the panel is actually showing "auto" weather.
            strncpy(liveWeather, value, sizeof(liveWeather) - 1);
            liveWeather[sizeof(liveWeather) - 1] = 0;
            if (dispState.cmd != DisplayCmd::Weather ||
                strcmp(dispState.weather.condition, "auto") != 0) return;
            s = dispState;   // re-enter auto weather so the scene picks up the change
        } else return;
        // Speed/mode changes re-apply the current display (a playing animation
        // restarts) — acceptable, and it keeps every state topic in sync.
        Command c{}; c.type = CmdType::Display; c.display = s;
        xQueueSend(cmdQueue, &c, 0);
    });

    if (mqttConfigured()) {
        mqtt.begin(cfg.mqttBroker, cfg.mqttPort, cfg.mqttUser, cfg.mqttPass, cfg.deviceName);
    }

    // Seed the WS state cache so the very first browser to connect sees the
    // real boot state (off, saved brightness) instead of nothing.
    webUI.broadcastState(dispState);

    Serial.printf("Ready. IP: %s\n", WiFi.localIP().toString().c_str());
}

// Drain the command queue — runs only in the loop() task, so all renderer /
// chain / cfg / dispState / MQTT access happens in one context.
static void processCommands() {
    Command c;
    while (cmdQueue && xQueueReceive(cmdQueue, &c, 0) == pdTRUE) {
        switch (c.type) {
        case CmdType::Display:
            dispState = c.display;
            applyDisplayState(c.display);
            cfg.brightness = c.display.brightness;
            configDirty = true; configDirtyAt = millis();   // debounced save
            publishHaStates();
            webUI.broadcastState(c.display);
            break;
        case CmdType::Config: {
            cfg = c.config;
            normalizeLayout(cfg);                            // clamp + shift to (0,0)
            normalizeZones(cfg);                              // enforce one-zone-per-family
            Storage::saveConfig(cfg);                        // explicit save, immediate
            uint16_t cw, ch;
            layoutBounds(cfg, &cw, &ch);
            delete renderer; renderer = new Renderer(cw, ch, cfg.layout, cfg.numPanels);
            delete chain;    chain    = new PanelChain(cfg.numPanels);
            chain->begin();
            applyRendererOptions();                          // gamma toggle takes effect live
            applyLayoutDerived();                            // effect buffers + centered-content rect
            applyTimeConfig();                               // tz change takes effect live
            dispState.brightness = cfg.brightness;
            // Re-render whatever was showing — the fresh renderer starts blank,
            // which used to leave a static image dark until it was re-sent.
            applyDisplayState(dispState);
            applyZonesFromConfig();                           // re-seed every zone (numPanels/zone_mode may have changed)
            webUI.broadcastZones(cfg);
            // (Re)initialize MQTT so enabling it or changing the broker takes effect
            // immediately, without a reboot. begin() disconnects any old session first.
            if (mqttConfigured())
                mqtt.begin(cfg.mqttBroker, cfg.mqttPort, cfg.mqttUser, cfg.mqttPass, cfg.deviceName);
            break;
        }
        case CmdType::ZoneSet: {
            if (c.zSlot < MAX_PANELS) {
                cfg.zoneSlots[c.zSlot].mode = c.zMode;
                strncpy(cfg.zoneSlots[c.zSlot].sub, c.zSub, sizeof(cfg.zoneSlots[c.zSlot].sub) - 1);
                normalizeZones(cfg);   // may revert THIS slot back to Off if another
                                       // zone already owns that Weather/Effect/Game family
                configDirty = true; configDirtyAt = millis();
                zoneApply(c.zSlot, (ZoneMode)cfg.zoneSlots[c.zSlot].mode, cfg.zoneSlots[c.zSlot].sub);
                canvasDirty = true;
                webUI.broadcastZones(cfg);
            }
            break;
        }
        case CmdType::Pixel:
            if (renderer) {
                renderer->setPixel(c.px, c.py, c.pv);
                canvasDirty = true;   // flushed by the next loop() pass (≤50 ms)
            }
            break;
        case CmdType::FilesChanged: {
            if (mqttConfigured()) mqtt.publishFileLists();
            // If the file currently on screen was just deleted, playback would
            // keep reading a stale (orphaned) handle while the UI and HA still
            // said it was playing — switch off cleanly instead. Uploads pass the
            // exists() check untouched. __sweep__ has no file, so skip it.
            char path[80]; bool gone = false;
            if (dispState.cmd == DisplayCmd::Image && dispState.image.name[0]) {
                snprintf(path, sizeof(path), "%s/%s", IMAGE_DIR, dispState.image.name);
                gone = !LittleFS.exists(path);
            } else if (dispState.cmd == DisplayCmd::Animation && effect == Effect::None) {
                snprintf(path, sizeof(path), "%s/%s", ANIM_DIR, dispState.anim.name);
                gone = !LittleFS.exists(path);
            }
            if (gone) {
                dispState.cmd   = DisplayCmd::Off;
                dispState.power = false;
                applyDisplayState(dispState);
                publishHaStates();
                webUI.broadcastState(dispState);
            }
            break;
        }
        case CmdType::DeleteFile: {
            // The HTTP handler couldn't remove the file because it's open — it's
            // the one currently playing. Stop playback (closes the handle), then
            // finish the delete and refresh the MQTT lists.
            bool current =
                ( c.isAnim && dispState.cmd == DisplayCmd::Animation &&
                  strcmp(dispState.anim.name,  c.fileName) == 0) ||
                (!c.isAnim && dispState.cmd == DisplayCmd::Image &&
                  strcmp(dispState.image.name, c.fileName) == 0);
            if (current) {
                dispState.cmd   = DisplayCmd::Off;
                dispState.power = false;
                applyDisplayState(dispState);
                publishHaStates();
                webUI.broadcastState(dispState);
            }
            char path[80];
            snprintf(path, sizeof(path), "%s/%s",
                     c.isAnim ? ANIM_DIR : IMAGE_DIR, c.fileName);
            LittleFS.remove(path);
            if (mqttConfigured()) mqtt.publishFileLists();
            break;
        }
        }
    }

    // Debounced config save from display commands (brightness, mode changes).
    if (configDirty && millis() - configDirtyAt > 1500) {
        configDirty = false;
        Storage::saveConfig(cfg);
    }
}

// --- Loop ---
void loop() {
    ArduinoOTA.handle();
    // Only service MQTT when it's configured — otherwise mqtt.loop() retries a
    // connection to an empty broker every 5s and the blocking attempt stalls the
    // frame updates (visible as a periodic pause in animations).
    if (mqttConfigured()) mqtt.loop();
    processCommands();    // drain queue — all state mutation happens here

    // Controller events → the running game (loop task owns all game state).
    // z<0 targets the legacy whole-canvas game; z>=0 targets that zone's game
    // (only meaningful while zone mode is on — silently dropped otherwise, so
    // stale input from a play-page tab that was mid-zone-game before an admin
    // switched back to canvas mode can't leak into the whole-canvas game).
    GameInputEvt evt;
    while (gameInQueue && xQueueReceive(gameInQueue, &evt, 0) == pdTRUE) {
        if (evt.z >= 0) {
            if (cfg.zoneModeOn) zoneInputRoute((uint8_t)evt.z, evt.p, evt.k, evt.s != 0);
        } else if (!cfg.zoneModeOn && dispState.cmd == DisplayCmd::Game) {
            gameInput(evt.p, evt.k, evt.s != 0);
        }
    }

    unsigned long now = millis();

    if (cfg.zoneModeOn) {
        // Each physical panel runs its own content — advance every configured
        // zone and composite it into the shared canvas. Only while powered on;
        // otherwise leave the canvas as applyDisplayState's off-command left it
        // (blank) so the master Power toggle still works in zone mode instead
        // of being immediately overwritten by the next zone blit.
        if (dispState.power) {
            bool any = false;
            for (uint8_t k = 0; k < cfg.numPanels && k < MAX_PANELS; k++) {
                if (zoneTick(k, now)) any = true;
                renderer->blitPanelZone(k, zoneCanvas(k).canvas());
            }
            if (any) canvasDirty = true;
        }
    } else {
        tickDisplay();
        // Pip, the idle critter — an overlay pass after content rendering, so he
        // walks in front of whatever is showing (and it heals behind him).
        if (cfg.critterOn &&
            critterService(*renderer, displayCalm(), crX, crY, crW, crH, contentEpoch, now))
            canvasDirty = true;
    }

    // Global brightness → hardware OE PWM via CMD_BRIGHTNESS. Content is rendered
    // at full scale; the panels dim uniformly, preserving 8-bit grayscale.
    // Sent on change, and re-sent every 1 s so a panel that reset (watchdog/power)
    // picks the level back up instead of staying at its full-brightness default.
    if ((int)dispState.brightness != sentBrightness || now - lastBrightSend >= 1000) {
        sentBrightness = dispState.brightness;
        lastBrightSend = now;
        chain->setBrightness(dispState.brightness);
    }

    // Networked games (Raid 16) publish ~10 Hz idempotent deck-state snapshots
    // over the WS. Only whole-canvas games — a zone-hosted game has no deck UI.
    if (!cfg.zoneModeOn && dispState.cmd == DisplayCmd::Game && now - lastGameNet >= 100) {
        lastGameNet = now;
        // static: keeps the loop-task stack small. GAME_NET_BUF is the
        // publisher contract — an undersized buffer makes gameNetSnapshot
        // return 0, which is indistinguishable from "nothing to send".
        static char netBuf[GAME_NET_BUF];
        size_t n = gameNetSnapshot(netBuf, sizeof(netBuf));
        if (n) webUI.broadcastText(netBuf, n);
    }

    // Reap dead/stuck WebSocket clients (they otherwise pin their queued frames
    // on the heap until TCP timeout — a slow leak under flaky clients).
    if (now - lastWsService >= 1000) {
        lastWsService = now;
        webUI.service();
    }

    if (now - lastFlush >= 50) {   // ~20fps max push rate
        lastFlush = now;
        // Push only when the canvas actually changed, plus a 1 Hz keepalive so a
        // panel that reset (watchdog/power blip) recovers the current frame —
        // same rationale as the brightness resend above. Static content now
        // idles the UART instead of re-sending identical packets 20×/s.
        bool keepalive = now - lastKeepalive >= 1000;
        if (canvasDirty || keepalive) {
            lastKeepalive = now;
            // In zone mode, dispState.cmd is irrelevant to what's on screen
            // (zones own their own content) — only the master Power toggle
            // matters. Outside zone mode this is exactly the original check.
            bool showing = dispState.power && (cfg.zoneModeOn || dispState.cmd != DisplayCmd::Off);
            if (showing) {
                chain->flush(*renderer);
            } else {
                chain->clearAll();
            }
            // The WS preview only needs real changes — new clients get the cached
            // last frame on connect, so keepalive resends would just burn WiFi.
            if (canvasDirty) {
                webUI.broadcastFrame(renderer->canvas(),
                                     (uint8_t)renderer->width(),
                                     (uint8_t)renderer->height());
            }
            canvasDirty = false;
        }
    }

    // Yield to the idle/WiFi tasks instead of busy-spinning at 100% CPU; 1 ms
    // tick granularity is ample for every timer above (minimum period 10 ms).
    delay(1);
}

// --- Display engine ---

static void applyDisplayState(const DisplayState& s) {
    // scrollX/scrollY get (re)computed by resetScrollPosition() the next time
    // tickDisplay() sees scrollDirty — no need to reset them here too.
    scrollDirty  = true;
    canvasDirty  = true;
    contentEpoch++;
    animFrameIdx = 0;
    effect       = Effect::None;
    if (animFile) animFile.close();
    renderer->clear();

    if (!s.power || s.cmd == DisplayCmd::Off) {
        chain->clearAll();
        return;
    }

    // Built-in effects (and the __sweep__/__identify__ helpers) — no file,
    // generated live in tickDisplay by the effects module.
    if (s.cmd == DisplayCmd::Animation && strncmp(s.anim.name, "__", 2) == 0) {
        effect = effectByName(s.anim.name);
        if (effect != Effect::None) {
            effectInit(effect, *renderer);
            return;
        }
        // Unknown __name__: fall through to the file path (a hidden file like
        // the Draw tab's __preview__.anim is a real animation on flash).
    }

    if (s.cmd == DisplayCmd::Weather) {
        // "auto" tracks the live sky Home Assistant pushes; anything else is a
        // literal condition (manual pick / preview from the web UI).
        const char* cond = strcmp(s.weather.condition, "auto") == 0
                         ? liveWeather : s.weather.condition;
        weatherScene = weatherByCondition(cond);
        weatherInit(weatherScene, *renderer);
        return;
    }

    if (s.cmd == DisplayCmd::Game) {
        gameStart(s.game.name, *renderer);   // unknown name → black; parse validates
        return;
    }

    if (s.cmd == DisplayCmd::Image) {
        // .raw images: 8-bit grayscale, 1 byte/pixel, canvas-sized (the web UI
        // converts uploads to the current canvas dimensions).
        char path[80];
        snprintf(path, sizeof(path), "%s/%s", IMAGE_DIR, s.image.name);
        File f = LittleFS.open(path, "r");
        if (f) {
            int totalPx = (int)renderer->width() * renderer->height();
            uint8_t* buf = (uint8_t*)calloc(totalPx, 1);   // zero-fill: a short/old
            if (buf) {                                     //   file leaves the rest black
                size_t got = f.read(buf, totalPx);
                if (got == (size_t)totalPx)                // ignore wrong-sized files
                    renderer->blitImage(0, 0, buf, renderer->width(), renderer->height());
                free(buf);
            }
            f.close();
        }
    }

    if (s.cmd == DisplayCmd::Animation) {
        char path[80];
        snprintf(path, sizeof(path), "%s/%s", ANIM_DIR, s.anim.name);
        animFile = LittleFS.open(path, "r");
        free(animDelays); animDelays = nullptr;
        animNumFrames = 0;
        if (animFile) {
            AnimHeader hdr = {};
            size_t got = animFile.read((uint8_t*)&hdr, sizeof(hdr));
            uint32_t frameBytes = (uint32_t)hdr.target_w * hdr.target_h;
            size_t   expected   = sizeof(AnimHeader)
                                + (size_t)hdr.num_frames * frameBytes
                                + (size_t)hdr.num_frames * 2;
            // Validate everything playback depends on. Dims are capped at the
            // absolute canvas maxima (a corrupt 255×255 header would otherwise
            // demand a 65 KB frame buffer); a file recorded at a different
            // canvas shape still plays, blitted top-left and clipped. The size
            // check rejects truncated files so playback never reads past EOF.
            bool valid = got == sizeof(hdr)
                      && memcmp(hdr.magic, "ANIM", 4) == 0
                      && hdr.target_w >= 1 && hdr.target_w <= CANVAS_MAX_W
                      && hdr.target_h >= 1 && hdr.target_h <= CANVAS_MAX_H
                      && hdr.num_frames > 0
                      && animFile.size() >= expected;
            // One frame buffer per anim load (not per frame — that churned the
            // heap 50x/s before), sized for this file's own dimensions.
            free(animBuf); animBuf = nullptr;
            if (valid) animBuf = (uint8_t*)malloc(frameBytes);
            if (valid && animBuf) {
                animW          = hdr.target_w;
                animH          = hdr.target_h;
                animNumFrames  = hdr.num_frames;
                animFrameBytes = (uint16_t)frameBytes;  // 8-bit: 1 byte/pixel
                // Cache the delay table (2 bytes/frame, at the end of the file) —
                // saves a seek per frame during playback. On failure, playback
                // falls back to the command's frame_ms.
                size_t delayOffset = sizeof(AnimHeader) + (size_t)animNumFrames * animFrameBytes;
                size_t delayBytes  = (size_t)animNumFrames * 2;
                animDelays = (uint16_t*)malloc(delayBytes);
                if (animDelays) {
                    if (!animFile.seek(delayOffset) ||
                        animFile.read((uint8_t*)animDelays, delayBytes) != delayBytes) {
                        free(animDelays); animDelays = nullptr;
                    }
                }
            } else {
                animFile.close();
            }
        }
        animFrameIdx = 0;
    }
}

// Whether a mode uses the rotated (7w x 5h) glyph table vs. the upright (5w x 7h)
// one. The other axis of variation — travel direction / stacking axis — is handled
// directly by the switches below; with only 5 fixed modes (no diagonals) the
// stacking axis always matches the travel axis, so there's no need for the
// general dx/dy cascade math the 8-direction version required.
static bool scrollModeVertical(TextScrollMode m) {
    return m == TextScrollMode::VerticalNorth || m == TextScrollMode::VerticalSouth;
}

// Per-character step along the mode's stacking axis (glyph extent + 1px gap).
static int scrollCharStep(TextScrollMode m) {
    if (m == TextScrollMode::HorizontalWest) return 6;   // 5-wide glyph + gap
    return scrollModeVertical(m) ? 6 : 8;                // 5- or 7-tall glyph + gap
}

// Total span of the current scrollText along the stacking axis.
static int scrollTextSpan(TextScrollMode m) {
    int len = (int)strlen(scrollText);
    return len > 0 ? len * scrollCharStep(m) - 1 : 0;
}

// Places the text just off the edge it enters from (plus the configured loop
// gap), centered on the other axis. Travel spans the FULL canvas — including
// unmapped regions — so text scrolls "behind" a gap (e.g. a speaker module)
// with physically correct timing and re-emerges on the far side.
static void resetScrollPosition() {
    int panelW = renderer->width(), panelH = renderer->height();
    int gap = dispState.text.gapPx;
    TextScrollMode mode = dispState.text.scrollMode;

    switch (mode) {
    case TextScrollMode::HorizontalWest:
        scrollX = panelW + gap;           // enters from the right, travels left
        scrollY = (panelH - 7) / 2;       // vertically centered (7-tall glyph)
        break;
    case TextScrollMode::HorizontalNorth:
        scrollX = (panelW - 5) / 2;       // horizontally centered (5-wide glyph)
        scrollY = panelH + gap;           // enters from the bottom, travels up
        break;
    case TextScrollMode::HorizontalSouth:
        scrollX = (panelW - 5) / 2;
        scrollY = -scrollTextSpan(mode) - gap;  // enters from the top, travels down
        break;
    case TextScrollMode::VerticalNorth:
        scrollX = (panelW - 7) / 2;       // horizontally centered (7-wide rotated glyph)
        scrollY = panelH + gap;
        break;
    case TextScrollMode::VerticalSouth:
        scrollX = (panelW - 7) / 2;
        scrollY = -scrollTextSpan(mode) - gap;
        break;
    }
}

// Non-scrolling text. Horizontal modes lay the string along X (one line,
// clipping if too wide); vertical modes stack rotated glyphs top-to-bottom.
// Placement: the anchor override if set, otherwise centered on the largest
// contiguous panel rectangle — NOT the raw canvas, whose center may be a gap.
static void renderStaticText() {
    renderer->clear();
    int len = (int)strlen(scrollText);
    if (!len) return;
    int ax = dispState.text.anchorX, ay = dispState.text.anchorY;
    if (!scrollModeVertical(dispState.text.scrollMode)) {
        int w  = len * 6 - 1;
        int x0 = (ax >= 0) ? ax : crX + ((int)crW - w) / 2;
        int y0 = (ay >= 0) ? ay : crY + ((int)crH - 7) / 2;
        for (int i = 0; i < len; i++)
            renderer->drawChar(x0 + i * 6, y0, scrollText[i], 255);
    } else {
        int h  = len * 6 - 1;
        int x0 = (ax >= 0) ? ax : crX + ((int)crW - 7) / 2;
        int y0 = (ay >= 0) ? ay : crY + ((int)crH - h) / 2;
        for (int i = 0; i < len; i++)
            renderer->drawCharVertical(x0, y0 + i * 6, scrollText[i], 255);
    }
}

// Ends a one-shot display (non-loop animation, scroll-once text) through the
// normal state path so the web UI and HA see the change.
static void finishDisplay() {
    dispState.cmd   = DisplayCmd::Off;
    dispState.power = false;
    applyDisplayState(dispState);
    publishHaStates();
    webUI.broadcastState(dispState);
}

// Clock, placed on the largest contiguous panel rectangle (or the anchor
// override) so it never centers into a gap. Layout adapts to the space: HH:MM
// on one line when the rect is ≥26px wide (e.g. panels side by side),
// otherwise HH stacked over MM. Unsynced time shows "--" dashes.
static void renderClock(const struct tm& tmv, bool synced, bool blinkOn) {
    renderer->clear();
    char d[4] = {'-', '-', '-', '-'};   // HH MM digits
    bool pm = false;
    if (synced) {
        int hh = tmv.tm_hour, mm = tmv.tm_min;
        if (!dispState.clock.h24) {
            pm = hh >= 12;
            hh %= 12;
            if (hh == 0) hh = 12;
        }
        d[0] = (dispState.clock.h24 || hh >= 10) ? (char)('0' + hh / 10) : ' ';
        d[1] = (char)('0' + hh % 10);
        d[2] = (char)('0' + mm / 10);
        d[3] = (char)('0' + mm % 10);
    }
    int ax = dispState.clock.anchorX, ay = dispState.clock.anchorY;

    if (crW >= 26) {
        // Single line: HH : MM — digits at +0,+6, colon at +12..13, digits +15,+21 (26px, 7 tall)
        int x0 = (ax >= 0) ? ax : crX + ((int)crW - 26) / 2;
        int y0 = (ay >= 0) ? ay : crY + ((int)crH - 7) / 2;
        renderer->drawChar(x0,      y0, d[0], 255);
        renderer->drawChar(x0 + 6,  y0, d[1], 255);
        renderer->drawChar(x0 + 15, y0, d[2], 255);
        renderer->drawChar(x0 + 21, y0, d[3], 255);
        if (blinkOn) {
            renderer->setPixel(x0 + 12, y0 + 1, 255); renderer->setPixel(x0 + 13, y0 + 1, 255);
            renderer->setPixel(x0 + 12, y0 + 5, 255); renderer->setPixel(x0 + 13, y0 + 5, 255);
        }
        if (pm) renderer->setPixel(x0 + 25, y0 + 6, 180);
    } else {
        // Stacked: HH over MM in a 16x16 block
        int x0 = (ax >= 0) ? ax : crX + ((int)crW - 16) / 2;
        int y0 = (ay >= 0) ? ay : crY + ((int)crH - 16) / 2;
        renderer->drawChar(x0 + 2, y0 + 0, d[0], 255);
        renderer->drawChar(x0 + 8, y0 + 0, d[1], 255);
        renderer->drawChar(x0 + 2, y0 + 9, d[2], 255);
        renderer->drawChar(x0 + 8, y0 + 9, d[3], 255);
        if (blinkOn) {
            renderer->setPixel(x0 + 7, y0 + 7, 255); renderer->setPixel(x0 + 8, y0 + 7, 255);
            renderer->setPixel(x0 + 7, y0 + 8, 255); renderer->setPixel(x0 + 8, y0 + 8, 255);
        }
        if (pm) renderer->setPixel(x0 + 15, y0 + 15, 180);   // 12h PM dot, bottom-right
    }
}

static void tickDisplay() {
    unsigned long now = millis();
    if (!dispState.power || dispState.cmd == DisplayCmd::Off) return;

    if (effect != Effect::None && dispState.cmd == DisplayCmd::Animation) {
        if (now - lastAnimFrame >= dispState.anim.frameMs) {
            lastAnimFrame = now;
            effectTick(effect, *renderer, cfg, crX, crY, crW, crH);
            canvasDirty = true;
        }
        return;
    }

    if (dispState.cmd == DisplayCmd::Weather) {
        if (now - lastWeather >= 70) {   // ambient tick rate (fixed, not frameMs)
            lastWeather = now;
            weatherTick(weatherScene, *renderer);
            canvasDirty = true;
        }
        return;
    }

    if (dispState.cmd == DisplayCmd::Game) {
        if (gameTick(*renderer, now)) canvasDirty = true;   // games pace themselves
        return;
    }

    if (dispState.cmd == DisplayCmd::Clock) {
        static int  lastMin   = -1;
        static bool lastBlink = false;
        bool blink = (now / 1000) & 1;
        time_t tt = time(nullptr);
        struct tm tmv;
        localtime_r(&tt, &tmv);
        bool synced = (tmv.tm_year + 1900) >= 2020;   // pre-sync clock starts in 1970
        if (scrollDirty || tmv.tm_min != lastMin || blink != lastBlink) {
            scrollDirty = false;
            lastMin   = tmv.tm_min;
            lastBlink = blink;
            renderClock(tmv, synced, blink);
            canvasDirty = true;
            contentEpoch++;
        }
        return;
    }

    if (dispState.cmd == DisplayCmd::Text) {
        if (scrollDirty) {
            scrollDirty = false;
            strncpy(scrollText, dispState.text.text, sizeof(scrollText) - 1);
            resetScrollPosition();
            if (dispState.text.staticMode) {
                renderStaticText();   // rendered once; nothing moves after this
                canvasDirty = true;
                contentEpoch++;
            }
        }
        if (!dispState.text.staticMode && now - lastScroll >= dispState.text.scrollMs) {
            lastScroll = now;
            renderer->clear();

            TextScrollMode mode = dispState.text.scrollMode;
            bool vert = scrollModeVertical(mode);
            int  step = scrollCharStep(mode);
            int  len  = (int)strlen(scrollText);
            int  panelH = renderer->height();
            bool exited = false;

            if (mode == TextScrollMode::HorizontalWest) {
                int cx = scrollX;
                for (int i = 0; i < len; i++) {
                    renderer->drawChar(cx, scrollY, scrollText[i], 255);   // full-intensity content;
                    cx += step;                                            // global brightness is via OE PWM
                }
                scrollX--;
                exited = scrollX < -scrollTextSpan(mode);
            } else {
                int cy = scrollY;
                for (int i = 0; i < len; i++) {
                    if (vert) renderer->drawCharVertical(scrollX, cy, scrollText[i], 255);
                    else      renderer->drawChar(scrollX, cy, scrollText[i], 255);
                    cy += step;
                }
                bool north = (mode == TextScrollMode::HorizontalNorth || mode == TextScrollMode::VerticalNorth);
                if (north) {
                    scrollY--;
                    exited = scrollY < -scrollTextSpan(mode);
                } else {
                    scrollY++;
                    exited = scrollY > panelH;
                }
            }
            canvasDirty = true;
            if (exited) {
                if (dispState.text.once) { finishDisplay(); return; }
                resetScrollPosition();
            }
        }
    }

    if (dispState.cmd == DisplayCmd::Animation && animFile && animNumFrames > 0) {
        // Per-frame delay from the table cached at load; 0 (or a failed cache)
        // falls back to the command's frame_ms.
        uint16_t frameDelay = dispState.anim.frameMs;
        if (animDelays && animDelays[animFrameIdx] > 0) frameDelay = animDelays[animFrameIdx];

        if (now - lastAnimFrame >= frameDelay) {
            lastAnimFrame = now;

            // Blit using the anim's OWN dimensions, not the current panel count —
            // an anim recorded at a different panel count would otherwise make
            // blitImage read past the buffer. animBuf is static (the header
            // validation caps frames at its size); a short read (file changed
            // underneath us) skips the blit rather than showing stale bytes.
            size_t pixelPos = sizeof(AnimHeader) + (size_t)animFrameIdx * animFrameBytes;
            if (animBuf && animFile.seek(pixelPos) &&
                animFile.read(animBuf, animFrameBytes) == animFrameBytes) {
                renderer->blitImage(0, 0, animBuf, animW, animH);
                canvasDirty = true;
            }

            animFrameIdx++;
            if (animFrameIdx >= animNumFrames) {
                if (dispState.anim.loop) animFrameIdx = 0;
                else                     finishDisplay();
            }
        }
    }
}

// 16x16 1-bit wifi icon (arcs + dot), one uint16_t per row, bit 15 = leftmost.
static const uint16_t WIFI_ICON[16] = {
    0x0000, 0x0000, 0x0FF0, 0x300C, 0x4002, 0x07E0, 0x1818, 0x2004,
    0x03C0, 0x0420, 0x0000, 0x0180, 0x0180, 0x0000, 0x0000, 0x0000,
};

static void drawIcon16(const uint16_t* rows, uint8_t brightness) {
    renderer->clear();
    // Centered on the largest contiguous panel rect, not the raw canvas.
    int xBase = crX + ((int)crW - 16) / 2;
    int yBase = crY + ((int)crH - 16) / 2;
    for (int y = 0; y < 16; y++)
        for (int x = 0; x < 16; x++)
            if (rows[y] & (0x8000 >> x)) renderer->setPixel(xBase + x, yBase + y, brightness);
    chain->flush(*renderer);
}

static void setupWifi() {
    // The device is headless while connecting — show a wifi icon so a blank
    // panel is distinguishable from a dead one, and "AP" when the setup portal
    // opens so you know to go join Frekvens-Setup.
    drawIcon16(WIFI_ICON, 180);

    WiFiManager wm;
    wm.setHostname(HOSTNAME);
    wm.setConfigPortalTimeout(180);
    wm.setAPCallback([](WiFiManager*) {
        renderer->clear();
        int x = crX + ((int)crW - 12) / 2;
        int y = crY + ((int)crH - 7) / 2;
        renderer->drawChar(x,     y, 'A', 255);
        renderer->drawChar(x + 7, y, 'P', 255);
        chain->flush(*renderer);
    });
    if (!wm.autoConnect(AP_SSID, AP_PASSWORD)) {
        Serial.println("WiFi failed, restarting");
        ESP.restart();
    }
    WiFi.setHostname(HOSTNAME);
    renderer->clear();
    chain->flush(*renderer);   // connected — hand a clean canvas to loop()
}

static int otaBarCols = -1;   // last drawn progress-bar width, -1 = none

static void setupOTA() {
    ArduinoOTA.setHostname(HOSTNAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    // Progress bar on the panel during an OTA update. These callbacks run from
    // ArduinoOTA.handle() in the loop task (which blocks for the whole update),
    // so drawing + flushing directly here is safe and nothing else repaints.
    ArduinoOTA.onStart([]() {
        otaBarCols = -1;
        renderer->clear();
        chain->flush(*renderer);
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        int cols = total ? (int)((uint64_t)progress * crW / total) : 0;
        if (cols == otaBarCols) return;   // redraw only when the bar advances
        otaBarCols = cols;
        renderer->clear();
        int y = crY + crH / 2 - 1;
        for (int x = 0; x < cols; x++) {
            renderer->setPixel(crX + x, y,     255);
            renderer->setPixel(crX + x, y + 1, 255);
        }
        chain->flush(*renderer);
    });
    ArduinoOTA.begin();
}
