# Host tests

Parts of the firmware that are pure logic can be compiled and run on a normal
machine, with no hardware, no PlatformIO and no network. That makes the
expensive-to-observe behaviour (a 10-minute boss fight, a difficulty the team
rarely picks, a deck that drops out mid-fight) cheap to check.

```bash
./esp32/test/run.sh          # needs only g++
```

## What's covered

`raid_engine_test.cpp` — the Raid 16 fight engine (`../src/raid.cpp`). It links
the **real** `Renderer`, and supplies only the clock and RNG so runs are
deterministic. `raid.cpp` is `#include`d rather than linked so the test can
inspect its file statics directly.

- **Invariant sweep** — every boss × party size × difficulty, driven with
  random deck input on both a 16×16 and a 16×32 canvas. Asserts boss HP and
  hull never go negative, the state machine stays in range, and the ~10 Hz deck
  snapshot always fits the `GAME_NET_BUF` contract. (A snapshot that doesn't
  fit returns 0, which is indistinguishable from "nothing to send" and would
  silently freeze every deck.)
- **Terminal + phase deck events** — a kill emits `win`, a hull-out emits
  `wipe`, and crossing an HP gate emits `phase`.
- **DRILL lane determinism** — D1 never fires P3 lane shots, regardless of what
  the previous fight left in the engine's statics.
- **Disconnect pause** — a claimed deck going silent freezes the fight, and it
  resumes when the deck returns.
- **Assist governor** — arms after two consecutive wipes at D1–D2, grants
  +2 hull, and never arms at NIGHTMARE.

Two of these are regression pins for bugs this harness found: terminal/phase
events were being clobbered by the incidental damage event that caused them, and
`fLaneGap` was never reset per fight, so whether DRILL got lane fire depended on
leftover state from the previous fight.

## Adding a test

Anything that doesn't touch Arduino APIs can follow the same shape: add a
`.cpp`, declare whatever runtime stand-ins it needs (see `stub/Arduino.h`), and
add a build+run line to `run.sh`. Keep assertions named — the output is meant to
read as a checklist.
