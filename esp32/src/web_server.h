#pragma once
#include <stdint.h>
#include <string.h>
#include <functional>
#include "storage.h"

enum class DisplayCmd {
    Off,
    Text,
    Image,
    Animation,
    Clock,
    Weather,
    Game,
};

// Text scroll mode: a fixed set of 5 named (glyph orientation, travel direction)
// combos rather than an independent orientation x direction matrix. An earlier
// version supported 2 orientations x 8 directions (including diagonals), but the
// diagonal cascade math was fragile at 16px resolution and most combos looked odd
// or redundant in practice — these 5 are the ones that are actually useful.
enum class TextScrollMode : uint8_t {
    HorizontalWest,    // classic marquee: upright glyphs, side by side, scrolls left
    HorizontalNorth,   // upright glyphs, stacked vertically, scrolls up
    HorizontalSouth,   // upright glyphs, stacked vertically, scrolls down
    VerticalNorth,     // rotated glyphs, stacked vertically, scrolls up
    VerticalSouth,     // rotated glyphs, stacked vertically, scrolls down
};

struct TextParams {
    char           text[256]  = {};
    uint8_t        brightness = 255;
    uint16_t       scrollMs   = 80;
    TextScrollMode scrollMode = TextScrollMode::HorizontalWest;   // matches the original (only) behavior
    bool           staticMode = false;   // centered, no scrolling (direction ignored;
                                         //   vertical modes stack glyphs top-to-bottom)
    bool           once       = false;   // scroll through once, then switch off cleanly
    uint8_t        gapPx      = 0;       // extra blank travel between loop repeats
    // Static-text placement override (canvas px, top-left of the text block).
    // -1 = auto: centered on the largest contiguous panel rectangle, so text
    // doesn't center into a gap (e.g. a speaker module between two panels).
    int16_t        anchorX    = -1;
    int16_t        anchorY    = -1;
};

// Clock layout adapts to the arrangement: HH:MM on one line when the largest
// contiguous panel rectangle is wide enough (≥26px — e.g. two panels side by
// side), otherwise HH stacked over MM. Time comes from SNTP; AppConfig.tz
// supplies the POSIX timezone.
struct ClockParams {
    bool    h24     = true;   // false = 12-hour (no leading zero, PM dot)
    int16_t anchorX = -1;     // placement override, same semantics as TextParams
    int16_t anchorY = -1;
};

// Weather ambience. `condition` holds an HA weather.* value (e.g. "rainy") that
// selects the scene, OR the literal "auto" to track the live condition Home
// Assistant pushes over frekvens/weather/set. main.cpp resolves "auto".
struct WeatherParams {
    char condition[24] = "auto";
};

// Player-controlled games (games.h registry). Started from the play page
// ({"cmd":"game","name":"pong"}); controllers stream input events separately.
struct GameParams {
    char name[16] = {};
};

// String <-> enum helpers, shared between the WS JSON parser (web_server.cpp) and
// the MQTT display/set JSON parser (main.cpp) so both accept identical values.
inline TextScrollMode parseScrollMode(const char* s) {
    if (!s) return TextScrollMode::HorizontalWest;
    if (strcmp(s, "horizontal_north") == 0) return TextScrollMode::HorizontalNorth;
    if (strcmp(s, "horizontal_south") == 0) return TextScrollMode::HorizontalSouth;
    if (strcmp(s, "vertical_north")   == 0) return TextScrollMode::VerticalNorth;
    if (strcmp(s, "vertical_south")   == 0) return TextScrollMode::VerticalSouth;
    return TextScrollMode::HorizontalWest;   // default, and matches "horizontal_west"
}
inline const char* scrollModeToString(TextScrollMode m) {
    switch (m) {
        case TextScrollMode::HorizontalNorth: return "horizontal_north";
        case TextScrollMode::HorizontalSouth: return "horizontal_south";
        case TextScrollMode::VerticalNorth:   return "vertical_north";
        case TextScrollMode::VerticalSouth:   return "vertical_south";
        default:                              return "horizontal_west";
    }
}

// Filenames arriving from JSON / multipart / MQTT go straight into filesystem
// paths ("%s/%s") — reject anything that could escape the images/anims
// directories (slashes, ".."-style dotfiles) or produce an unopenable path.
// Shared by web_server.cpp's HTTP/WS handlers and main.cpp's MQTT parser.
inline bool validFileName(const char* n) {
    if (!n || !*n || strlen(n) > 40) return false;   // LittleFS caps basenames at 31 anyway
    if (n[0] == '.') return false;                    // no dotfiles, ".", ".."
    for (const char* p = n; *p; p++)
        if (*p == '/' || *p == '\\' || (uint8_t)*p < 0x20) return false;
    return true;
}

struct ImageParams {
    char    name[64] = {};
    uint8_t brightness = 255;
};

struct AnimParams {
    char     name[64]  = {};
    uint16_t frameMs   = 100;
    bool     loop      = true;
};

struct DisplayState {
    DisplayCmd    cmd       = DisplayCmd::Off;
    TextParams    text      = {};
    ImageParams   image     = {};
    AnimParams    anim      = {};
    ClockParams   clock     = {};
    WeatherParams weather   = {};
    GameParams    game      = {};
    bool          power     = true;
    uint8_t       brightness = 255;
};

using DisplayChangeCallback = std::function<void(const DisplayState&)>;
using ConfigSaveCallback    = std::function<void(const AppConfig&)>;
using RawPixelCallback      = std::function<void(int x, int y, uint8_t v)>;
using FilesChangedCallback  = std::function<void()>;
using DeleteFileCallback    = std::function<void(bool isAnim, const char* name)>;
// zoneSlot: -1 = legacy whole-canvas game (existing play.js behavior), 0..3 =
// target that zone's game (zone mode — see zones.h).
using GameInputCallback     = std::function<void(uint8_t player, char key, bool pressed, int8_t zoneSlot)>;
// mode is a raw ZoneMode value (zones.h) carried as uint8_t so this header
// doesn't need to depend on zones.h — mirrors AppConfig::ZoneSlot in storage.h.
using ZoneSetCallback       = std::function<void(uint8_t slot, uint8_t mode, const char* sub)>;

class WebUI {
public:
    WebUI() = default;

    void begin(AppConfig* cfg);

    void onDisplayChange(DisplayChangeCallback cb) { _displayCb = cb; }
    void onConfigSave(ConfigSaveCallback cb)        { _configCb  = cb; }
    void onSetPixel(RawPixelCallback cb)            { _pixelCb   = cb; }
    // Fired after an image/animation is uploaded or deleted, so main.cpp can
    // re-publish the MQTT file-list topics (frekvens/images, frekvens/anims).
    void onFilesChanged(FilesChangedCallback cb)    { _filesChangedCb = cb; }
    // Fired when a delete request hits a file LittleFS refuses to remove because
    // it's open — i.e. the animation currently playing. The loop task stops
    // playback (closing the handle) and finishes the delete.
    void onDeleteFile(DeleteFileCallback cb)        { _deleteCb = cb; }
    // Controller input from the play page ({"cmd":"input",...} WS messages).
    // Runs in the AsyncTCP task — main.cpp queues it for the loop task.
    void onGameInput(GameInputCallback cb)          { _gameInputCb = cb; }
    // A zone's mode/content assignment changed ({"cmd":"zone",...} WS message).
    void onZoneSet(ZoneSetCallback cb)              { _zoneSetCb = cb; }

    void broadcastState(const DisplayState& state);
    void broadcastFrame(const uint8_t* pixelData, uint8_t w, uint8_t h);
    // Zone-mode on/off + every zone's current assignment — cached and sent to
    // newly connected clients, same pattern as broadcastState/lastStateJson.
    void broadcastZones(const AppConfig& cfg);

    // Periodic housekeeping — call from loop() about once a second. Reaps
    // disconnected/stuck WebSocket clients, which otherwise linger with their
    // queued frames pinned on the heap (the slow drain seen under WS churn).
    void service();

    // Internal — left accessible so static callbacks in web_server.cpp can reach them
    AppConfig*            _cfg       = nullptr;
    DisplayChangeCallback _displayCb;
    ConfigSaveCallback    _configCb;
    RawPixelCallback      _pixelCb;
    FilesChangedCallback  _filesChangedCb;
    DeleteFileCallback    _deleteCb;
    GameInputCallback     _gameInputCb;
    ZoneSetCallback       _zoneSetCb;
};
