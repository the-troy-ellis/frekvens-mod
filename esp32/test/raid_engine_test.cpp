// Native regression test for the Raid 16 fight engine (esp32/src/raid.cpp).
//
// The engine is pure logic over a Renderer, so it runs on the host against the
// real Renderer plus a deterministic RNG and clock — no hardware, no
// PlatformIO, no network.  Build + run:  ./esp32/test/run.sh
//
// It drives complete fights across every boss × party size × difficulty and
// asserts the invariants that are expensive to notice on real hardware. Two
// real bugs were found this way and are now pinned by named cases below:
//   · terminal/phase events (win/wipe/phase) were clobbered by the incidental
//     damage event that caused them, so decks never saw them
//   · fLaneGap was never reset per fight, so whether DRILL got P3 lane fire
//     depended on leftover state from the PREVIOUS fight
//
// raid.cpp is #included (not linked) so the test can inspect its file statics.
// The REAL Renderer is compiled and linked in — the engine is exercised against
// production geometry/clipping, not a lookalike.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <set>

// --- deterministic stand-ins for the runtime the stub Arduino.h declares ---
static uint32_t rngState = 0x13579BDF;
uint32_t esp_random() {
    rngState ^= rngState << 13; rngState ^= rngState >> 17; rngState ^= rngState << 5;
    return rngState;
}
static uint32_t g_millis = 0;
unsigned long millis() { return g_millis; }

#include "../src/games.h"   // GAME_NET_BUF — the snapshot buffer contract
#include "../src/raid.cpp"

// A canvas of `panels` stacked 16x16 panels, matching the real chain layout.
static Renderer* makeCanvas(uint8_t panels) {
    static PanelPlacement layout[MAX_PANELS];
    for (uint8_t i = 0; i < MAX_PANELS; i++) layout[i] = { 0, (int16_t)(i * PANEL_HEIGHT), 0 };
    return new Renderer(PANEL_WIDTH, (uint16_t)(PANEL_HEIGHT * panels), layout, panels);
}

// ---------------------------------------------------------------------------
static int failures = 0;
static void check(bool ok, const char* what) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) failures++;
}

static Renderer* R = nullptr;
static std::set<std::string> ev;

// Advance the engine, recording every one-shot event the decks would receive.
static void step(int n) {
    for (int i = 0; i < n; i++) {
        g_millis += RD_TICKMS;
        raidTick(*R, g_millis);
        if (fEvName[0]) ev.insert(fEvName);
    }
}
static void heartbeatAll() { for (uint8_t i = 0; i < 4; i++) raidInput(i, 'H', true); }

// Enter a fight deterministically. fDiff is set directly because 'D' is a
// cycler, not a setter — driving it by keypress makes tests order-dependent.
// All four roles heartbeat in the lobby, which is how a real deck claims its
// role (raid.js heartbeats whenever it has a snapshot, lobby included).
static void startFight(char boss, int party, uint8_t diff) {
    ev.clear();
    fState = ST_IDLE;
    raidInput(0, 'G', true);              // idle -> lobby
    raidInput(0, boss, true);
    raidInput(0, (char)('0' + party), true);
    heartbeatAll();                       // claim all four decks
    fDiff = diff;
    raidInput(0, 'G', true);              // lobby -> intro
    // The intro is variable-length: the nameplate scroll is sized by the boss +
    // mood epithet (plus " ASSISTED"), then a 3-2-1 countdown. Pump until the
    // fight actually starts rather than guessing a tick count.
    for (int i = 0; i < 400 && fState == ST_INTRO; i++) step(1);
}
static void leaveFight() {
    step(200);
    raidInput(0, 'Q', true); step(3);     // -> lobby
    raidInput(0, 'Q', true); step(3);     // -> idle
}

// Block every telegraph correctly; never fire. Keeps a fight alive indefinitely.
static void playDefensively(int ticks) {
    for (int t = 0; t < ticks && fState == ST_FIGHT; t++) {
        g_millis += RD_TICKMS;
        raidTick(*R, g_millis);
        if (fEvName[0]) ev.insert(fEvName);
        if (fPhase == F_TELE) {
            if      (fType == T_SWEEP_L) raidInput(0, 'L', true);
            else if (fType == T_SWEEP_R) raidInput(0, 'R', true);
            else if (fType == T_BEAM)    raidInput(0, (char)('0' + fFreq), true);
            else if (fType == T_CHARGE)  raidInput(0, 'O', true);
            else if (fType == T_RIP)     raidInput(0, fRipSide == 1 ? 'L' : 'R', true);
        }
        if (fLane >= 0) raidInput((uint8_t)fLane, 'X', true);
        if (fAcid >= 0) raidInput(3, 'W', true);
        if (fJam  >= 0) raidInput(3, (char)('a' + fResync), true);
        heartbeatAll();
        if (fEvName[0]) ev.insert(fEvName);
    }
}

// ---------------------------------------------------------------------------
// 1. Sweep: every boss × party × difficulty, random deck mashing.
//    Asserts state/HP/hull invariants and that the deck snapshot always fits
//    the GAME_NET_BUF contract (returning 0 looks like "idle" and would
//    silently freeze every deck).
static void testSweep(uint8_t panels) {
    Renderer& r = *makeCanvas(panels);
    R = &r;
    raidInit(r);
    long hpNeg = 0, hullNeg = 0, badState = 0, netFail = 0, netTrunc = 0;
    size_t maxNet = 0;
    char prodBuf[GAME_NET_BUF], bigBuf[4096];
    const char* bosses = "VMB";

    for (int b = 0; b < 3; b++)
    for (int party = 1; party <= 4; party++)
    for (uint8_t diff = 0; diff < 4; diff++) {
        startFight(bosses[b], party, diff);
        for (int t = 0; t < 3000 && fState != ST_STATS; t++) {
            g_millis += RD_TICKMS;
            raidTick(r, g_millis);

            size_t big = raidNet(bigBuf, sizeof(bigBuf));
            size_t prod = raidNet(prodBuf, sizeof(prodBuf));
            if (big > maxNet) maxNet = big;
            if (big > 0 && prod == 0) netTrunc++;         // fits 4096, not the real buffer
            if (big == 0 && fState != ST_IDLE) netFail++;

            if (fHP   < 0) hpNeg++;
            if (fHull < 0) hullNeg++;
            if (fState > ST_STATS) badState++;

            static const char KEYS[] = "LR1234OKFPTUWabcdX";
            uint32_t k = esp_random() % (sizeof(KEYS) + 4);
            if (k < sizeof(KEYS) - 1)
                raidInput((uint8_t)(esp_random() % 4), KEYS[k], true);
            heartbeatAll();
        }
        leaveFight();
    }
    printf("  canvas 16x%d: maxSnapshot=%zu/%d bytes\n", r.height(), maxNet, GAME_NET_BUF);
    check(hpNeg == 0,    "boss HP never goes negative");
    check(hullNeg == 0,  "hull never goes negative");
    check(badState == 0, "fight state stays in range");
    check(netFail == 0,  "snapshot always produced while the fight flow is live");
    check(netTrunc == 0, "snapshot always fits GAME_NET_BUF");
}

// 2. Regression: a kill must emit "win", and crossing an HP gate must emit
//    "phase". Both used to be overwritten by the damage event that caused them.
static void testTerminalEvents() {
    Renderer& r = *makeCanvas(2);
    R = &r;
    raidInit(r);
    startFight('V', 1, 1);                 // VANTA, solo, FIELD
    for (int t = 0; t < 9000 && fState == ST_FIGHT; t++) {
        g_millis += RD_TICKMS;
        raidTick(r, g_millis);
        if (fEvName[0]) ev.insert(fEvName);
        if (fPhase == F_TELE) {
            if      (fType == T_SWEEP_L) raidInput(0, 'L', true);
            else if (fType == T_SWEEP_R) raidInput(0, 'R', true);
            else if (fType == T_BEAM)    raidInput(0, (char)('0' + fFreq), true);
            else if (fType == T_CHARGE)  raidInput(0, 'O', true);
        }
        raidInput(1, 'K', true);
        raidInput(1, 'F', true);           // hammer the gun until it dies
        heartbeatAll();
        if (fEvName[0]) ev.insert(fEvName); // inputs emit events too
    }
    check(fHP == 0,            "boss reaches 0 HP under sustained fire");
    check(ev.count("win"),     "a kill emits the 'win' deck event");
    check(ev.count("phase"),   "crossing an HP gate emits the 'phase' deck event");
    leaveFight();

    // And a wipe must emit "wipe" (was clobbered by "bosshit").
    ev.clear();
    startFight('V', 1, 1);
    for (int t = 0; t < 9000 && fState == ST_FIGHT; t++) {
        g_millis += RD_TICKMS;
        raidTick(r, g_millis);             // block nothing at all -> hull runs out
        if (fEvName[0]) ev.insert(fEvName);
        heartbeatAll();
    }
    check(ev.count("wipe"),    "running out of hull emits the 'wipe' deck event");
    leaveFight();
}

// 3. Regression: DRILL (D1) strips lane fire, and that must not depend on
//    whatever fLaneGap the previous fight happened to leave behind.
static void testDrillLaneDeterminism() {
    Renderer& r = *makeCanvas(2);
    R = &r;
    raidInit(r);
    int fired[2];
    for (int i = 0; i < 2; i++) {
        fState = ST_IDLE;
        raidInput(0, 'G', true);
        raidInput(0, 'V', true);
        raidInput(0, '4', true);           // party 4 — lanes are a 2p+ mechanic
        fDiff = 0;                         // DRILL
        fLaneGap = i ? 5 : 0;              // leftover from a previous fight
        raidInput(0, 'G', true);
        ev.clear();
        step(120);
        fHP = 20; checkPhase();            // force the P3 gate
        for (int t = 0; t < 600 && fState == ST_FIGHT; t++) {
            g_millis += RD_TICKMS;
            raidTick(r, g_millis);
            if (fEvName[0]) ev.insert(fEvName);
            heartbeatAll();
        }
        fired[i] = (int)ev.count("lane");
        leaveFight();
    }
    check(fired[0] == 0 && fired[1] == 0,
          "DRILL never fires lane shots regardless of leftover state");
    check(fired[0] == fired[1],
          "lane behaviour is independent of the previous fight");
}

// 4. Disconnect-pause: a claimed deck that goes silent freezes the fight, and
//    the fight resumes (and the snapshot reports it) once the deck returns.
static void testDisconnectPause() {
    Renderer& r = *makeCanvas(2);
    R = &r;
    raidInit(r);
    startFight('V', 4, 1);
    playDefensively(60);                    // all four decks claimed + alive
    int hpBefore = fHP, hullBefore = fHull;

    for (int t = 0; t < 200; t++) {         // role 2 goes quiet, the rest keep talking
        g_millis += RD_TICKMS;
        raidTick(r, g_millis);
        for (uint8_t i = 0; i < 4; i++) if (i != 2) raidInput(i, 'H', true);
    }
    check(fPausedRole == 2, "a silent claimed deck pauses the fight");
    check(fHP == hpBefore && fHull == hullBefore, "no fight progress while paused");

    for (int t = 0; t < 20; t++) {          // it comes back
        g_millis += RD_TICKMS;
        raidTick(r, g_millis);
        heartbeatAll();
    }
    check(fPausedRole == -1, "fight resumes when the deck returns");
    leaveFight();
}

// 5. The assist governor arms after two consecutive wipes at D1-D2, and a win
//    clears the streak.
static void testAssistGovernor() {
    Renderer& r = *makeCanvas(2);
    R = &r;
    raidInit(r);
    memset(fWipeStreak, 0, sizeof(fWipeStreak));

    for (int i = 0; i < 2; i++) {           // wipe twice: block nothing
        startFight('V', 1, 1);
        for (int t = 0; t < 9000 && fState == ST_FIGHT; t++) {
            g_millis += RD_TICKMS; raidTick(r, g_millis); heartbeatAll();
        }
        check(fStats.res && strcmp(fStats.res, "wipe") == 0, "passive play wipes");
        leaveFight();
    }
    startFight('V', 1, 1);
    check(fAssist, "assist governor arms after two consecutive wipes at FIELD");
    check(fHull == DIFFS[1].hull + 2, "assist grants +2 hull");
    leaveFight();

    // At NIGHTMARE the governor must stay off however badly it goes.
    memset(fWipeStreak, 0, sizeof(fWipeStreak));
    fWipeStreak[0] = 5;
    startFight('V', 1, 3);
    check(!fAssist, "assist governor never arms at NIGHTMARE");
    leaveFight();
}

int main() {
    printf("raid engine — invariant sweep\n");
    testSweep(1);                           // 16x16: no stage row (decks carry the bars)
    testSweep(2);                           // 16x32: the design's reference canvas
    printf("raid engine — terminal + phase deck events\n");
    testTerminalEvents();
    printf("raid engine — DRILL lane determinism\n");
    testDrillLaneDeterminism();
    printf("raid engine — disconnect pause\n");
    testDisconnectPause();
    printf("raid engine — assist governor\n");
    testAssistGovernor();

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all checks passed",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
