# RAID 16 — design + technical spec (v0.2)

Asymmetric co-op boss battles for **1–4 players**. The Frekvens array IS the
boss; phones are the control decks. Players read telegraphs on the physical
display, shout what they see to the teammate whose deck can act on it, and
punish clean blocks. Spaceteam-style panic, but the chaos source is a living
pixel titan on the shelf.

v0.2 expands v0.1 (single boss, 2–4p) into the full game: a five-boss roster
with distinct personalities and strategies, true 1–4 player scaling (solo is
now supported — see §4), per-role deck visual design, a five-detent difficulty
knob, and a four-layer variance engine that makes every fight play out
differently.

---

## 0. Reality checks (unchanged from v0.1 — these are settled)

1. **Monochrome panel.** No color mechanics. Frequency (blink cadence),
   shape, and motion are the boss's entire language. LED-legibility rules
   apply: ≤3 brightness levels (dim 30–70 / body 200 / highlight 255),
   silhouettes not gradients, blinking not shimmer.
2. **No gyroscope** (iOS needs HTTPS; we serve HTTP). All inputs are touch:
   cranks, dials, rockers, traces, scrubs.
3. **Canvas is 16×32** (2-panel stack): top panel = boss face, bottom = stage
   (hull bar, lanes, projectiles). Adapts down to a single 16×16 (stage
   collapses into the face's bottom rows) and up to future arrangements.
4. **Phones never mirror the display mid-fight.** Look up — that's the game.
5. **No device audio; Home Assistant is the sound system.** MQTT events
   (`frekvens/raid/event`) → HA automation → room speakers.
6. **ESP32 is the master clock.** 20 Hz state machine, 10 Hz idempotent JSON
   snapshots to all decks, edge-triggered inputs back. No client simulation.

---

## 1. Design pillars

- **The panel is sacred.** Anything expressible in 16×16 lives on the panel.
  Phones hold only what a face cannot say (dials, inventories, code streams).
- **Nobody acts alone** (at 2p+). Every meaningful action needs information
  or a resource from a different deck. The game is 90% shouting.
- **One alphabet, many dialects.** All bosses speak the same telegraph
  vocabulary (§2) — reading skill transfers — but each boss bends it with a
  signature mechanic that demands a different *strategy*, not just faster
  reflexes.
- **A fight is 8–12 minutes.** A gauntlet run is 35–50. Short enough for
  "one more wipe."
- **Variance is systemic, not cosmetic.** Decks-of-attacks, moods, mutations,
  and adaptive drift (§7) — not scripts.

---

## 2. The shared telegraph alphabet

Every boss draws from this table. New bosses add *dialect* (weights, feints,
sequences), never new grammar — that's what keeps a 16×16 face readable.

| Telegraph (on the panel)                    | Meaning                   | Correct response                                    | Base window |
|---------------------------------------------|---------------------------|-----------------------------------------------------|-------------|
| Eyes slide hard L/R + edge ripple            | **Sweep laser** that side | Shield: side rocker to match                        | 2.5 s |
| Eyes blink steady loop of N (1–4)            | **Beam, frequency N**     | Hacker decodes N → Shield dials N                   | 4 s |
| Mouth opens stage 1→2→3                      | **Charge blast**          | Gunner interrupts with loaded shell OR Shield overcharge | 3 s |
| Mouth spews falling glops into a lane        | **Acid** on that deck     | Medic scrub-wipes that deck                          | until wiped |
| Face dissolves toward static                 | **Jam** — a deck scrambles| Medic re-sync (key shown on Hacker's deck)           | until fixed |
| Jagged grin + fast flicker                   | **Enrage tell**           | none — dps race                                      | — |
| Face melts downward                          | **Death**                 | cheer                                                | — |

Failing costs hull: sweep −1, beam −2, charge −3; acid/jam disable decks
(indirect). A **clean frequency-matched block** opens a vulnerability window
(mouth hangs open ~3 s, Gunner damage ×3). Chip damage cannot beat the enrage
timer — windows are the win condition.

---

## 3. The boss roster

Five bosses, five strategic verbs: **LEARN → PATIENCE → PRIORITIZE → BAIT →
ADAPT.** Each has a face anatomy program (parametric, no sprite sheets), an
intro nameplate (name + mood epithet scrolls the panel), phase taunts, and a
bespoke death animation. Stats below are 4p/Field-difficulty baselines; §4
and §6 scale them.

### 3.1 VANTA, the Signal Titan — *the gatekeeper* (LEARN)
- **HP 100 · 3 phases · cadence 8–10 s → 6–8 s → 4–6 s**
- The v0.1 boss, textbook dialect: honest telegraphs, generous windows,
  acid+jam in P2, bullet-hell + 90 s enrage in P3.
- **Personality:** cold, unblinking, methodical. Idle: slow gaze drift, rare
  double-blink. Taunt: stares directly out, brow flattens.
- **Face:** the standard anatomy (3×3 eyes w/ pupil holes, staged mouth).
- **Death:** the classic row-by-row melt into a puddle.
- **Moods (§7.2):** COLD (baseline) / CURIOUS (more beams, windows +15%) /
  STERN (more sweeps, windows −15%).

### 3.2 MOTH, the Feintweaver — *don't overcommit* (PATIENCE)
- **HP 85 · 3 phases · faster cadence (6–8 s), weaker hits (sweep −1, beam −1)**
- **Signature — FEINTS:** 30–45% of telegraphs dissolve at ~60% completion.
  Committing the shield to a feint costs a 3 s shield cooldown — the punish
  is for *guessing*. The Hacker's telemetry gains a pulse readout: during any
  telegraph the live line flickers if it's fake → the Hacker's job becomes
  shouting **"REAL!" / "FAKE!"** under time pressure. Solo/1-deck parties
  read the flicker themselves (§4).
- **Signature — DUST:** wing flaps scatter drifting motes across the stage
  that hide lane markers until a Gunner shot or Medic gust clears them.
- **Personality:** twitchy, flittering, never still. Idle: wings (edge
  columns) flutter, compound eyes shimmer in patches. Taunt: false-starts a
  charge then giggle-flutters.
- **Face:** compound anatomy — two 4×4 eye grids of independently blinking
  cells, moth "antennae" rows at the top, wing columns at the panel edges.
- **Death:** disintegrates into motes that flutter up and off the top edge.
- **Moods:** SKITTISH (feint rate 45%, damage low) / FRENZIED (cadence +20%,
  feints 30%) / SLY (feints hide inside double telegraphs).

### 3.3 THE CHORUS, Three-as-One — *pick your target* (PRIORITIZE)
- **Three heads, 35 HP each · phases emerge from kills, not HP gates**
- **Face:** triple-band anatomy — the top panel splits into three stacked
  5-row mini-faces (brow / eye pair with pupils / mouth line). The leading
  head's band edge glows.
- **Signature — ROUNDS:** attacks chain across heads in announced order
  (lead head telegraphs first; L→M→R arrows tick across its band). Blocks
  must land in sequence — the Shield's rocker and dial get queued shouts
  ("LEFT THREE, THEN RIGHT ONE!").
- **Signature — TARGETING:** the Gunner's aim rocker selects a *head*, not a
  side. Kill order is strategy: each dead head silences its attacks but the
  survivors **harmonize** (cadence +25% each death, windows −10%). Spread
  damage = slower, safer; focus = fast but the last head is a monster.
- **Personality:** they bicker. Idle: heads glance at each other, one rolls
  its eyes (pupil loop). Taunt: all three grin in unison — the only time
  they agree.
- **Death:** heads pop one by one; the last hangs alone for a beat, looks
  left and right for its siblings, then deflates.
- **Moods:** SOPRANO (top head leads, fast rounds) / BASSO (bottom leads,
  heavy hits) / ROUND (lead rotates every chain — hardest to track).

### 3.4 BULWARK, the Sealed Door — *make it open up* (BAIT)
- **HP 70, armored: all damage capped at 1 while the visor is down**
- **Signature — RIPOSTE LOOP (inverts the core loop):** the team must
  *provoke*: Gunner fires a deliberate light **PING shell** → Bulwark
  ripostes with a fast counter-telegraph (1.8 s window) → clean block →
  **visor lifts 4 s** (heavy shells ×3). Attack → defend → punish, instead of
  defend → punish.
- **Two shell types:** Medic's forge gains a fork — 2-node trace = ping,
  4-node = heavy. The breach holds one of each; the Gunner must fire the
  right one at the right time ("PING NOW — HEAVY'S FOR THE WINDOW!").
- **Signature — BASH:** periodically rams the screen edge; both panels
  shudder (rows shift 1 px for 3 ticks) and every deck's controls physically
  displace for ~2 s (cosmetic at low difficulty, briefly remapped at D4+).
- **Personality:** patient, contemptuous. Idle: visor slits track players
  slowly; occasionally "knocks" from inside (center pixels pulse). Taunt:
  visor cracks open a pixel, peeks, slams shut.
- **Face:** standard anatomy under a visor overlay — horizontal armor slats
  with eye-slits; the lift animation raises the slats row by row.
- **Death:** visor falls off (slides down the panel and off), the bare face
  looks genuinely surprised, then crumbles from the edges inward.
- **Moods:** PATIENT (baseline) / WRATHFUL (bash more, riposte harder) /
  MOCKING (fake visor-lifts that snap shut — bait the baiter).

### 3.5 NULL, the Static King — *trust nothing* (ADAPT) — finale
- **HP 120 · 4 phases · unlocked in Quick Fight after first gauntlet clear**
- **Face:** noise anatomy — a face that only barely coheres out of static
  (30–60% noise floor); features assemble when it attacks and dissolve after.
- **Signature — CORRUPTION, escalating by phase:**
  - **P1 Poisoned telemetry:** one Hacker line always lies. The panel's blink
    count is ground truth — the Hacker must cross-check and call out which
    line is poisoned this fight.
  - **P2 Possession:** NULL's eye appears on a random deck; that deck's
    inputs now *feed the boss* (blocks weaken, cranks heal it). Its owner
    must sit on their hands and shout while the Medic runs the exorcise
    mini-game on it.
  - **P3 Inversion:** the panel image flips upside-down for 10 s stretches —
    sweeps and blink codes must be read inverted (hilarious, legal, brutal).
  - **P4 The false wipe:** once, at ~15% HP, NULL performs a fake death melt
    then reforms at random within 3–8 s and immediately attacks. Teams that
    celebrate early eat a free charge blast.
- **Dialect theft:** NULL has no mood — each fight it samples one signature
  from two other bosses in corrupted form (feints that jam, ripostes that
  possess…). The fiction: NULL is the signal behind all of them.
- **Personality:** it cheats, and it knows you know. Idle: static breathes;
  occasionally forms a perfect copy of ANOTHER boss's face for one second.
- **Death:** static collapses inward to a single center pixel that blinks
  like a CRT powering off… then one last faint blink after 3 s of black.

---

## 4. Party scaling (1–4 players)

Decks are bundles of the four consoles; consoles never disappear, they
consolidate. The deck app stacks half-height panels when one player holds two.

| Players | Decks | What changes |
|---------|-------|--------------|
| **1 — SOLO SIM** | The **Pilot deck**: all four consoles condensed into quadrants | The decode layer is removed — this is the honest design change that makes solo real: the boss blinks its frequency openly (no Hacker relay), forge is a single hold (no send tube), scan is one button. Solo becomes a *panel-attention* game: read up, act down. Framed in-fiction as the **training simulator** — which also justifies practicing any boss. Feints/possession retuned (they target your one deck, gently). |
| **2** | Info deck (Hacker+Shield) · Action deck (Medic+Gunner) | Decode layer ON — the two-player shout ("freq three!" / "shell's loaded!") is the core. Acid/jam rate halved. |
| **3** | Shield · Hacker · Gunner+Medic | The forge→load pipeline is one person's rhythm. |
| **4** | One console each | The intended experience. Full chaos. |

Auto-normalization (applied under any difficulty): cadence ×0.5 / 0.65 /
0.8 / 1.0 for 1/2/3/4p; simultaneous-threat cap 1 / 1 / 2 / 3; Chorus rounds
max length 2 / 2 / 3 / 3.

**Disconnects:** 10 s grace (boss idles, suspicious face), then pause with
the boss visibly asleep until the deck rejoins (localStorage token restores
the role). Screen-lock should never wipe a run.

---

## 5. The decks — visual identity per role

Shared frame on every deck (the "cockpit header"): boss nameplate + mood
epithet, phase pips, hull LED row (mirrors the panel's hull bar), **your
lane tag** (matches your stage lane marker), and the status LED chip. All of
it in the TE instrument language the web UI already speaks — key-caps with
press travel, one signal orange, silkscreen micro-labels, numbered modules.

### 🛡 Shield Engineer — "the breaker panel"
Heavy-switchgear aesthetic: hazard-striped header, a **giant two-pane rocker**
(L/R) that physically see-saws (CSS transform on press), a **rotary frequency
selector** (drag through detents 1–4, notched like a real range knob), and an
**overcharge handle** — a wide pull-bar you hold with both thumbs; a
capacitor gauge drains beside it during cooldown. Idle animation: tiny
"grid hum" flicker on the fuse row. When a block lands clean, the whole deck
edge-flashes once — the deck itself celebrates.

### 🔫 Weapons Loader — "the artillery station"
Ordnance-stencil aesthetic. A **flywheel crank** (the conic-gradient dial
from the concept demo, upgraded with ratchet tick marks and momentum — it
keeps spinning briefly after release), the **breach** rendered as a
side-view shell tray (ping shells short/stubby, heavies long — Bulwark fights
show both slots), an **aim rocker** with a 3-lamp target strip (Chorus fights
relabel it TOP/MID/BOT), and a guarded **FIRE** key under a flip-cover you
swipe open first (prevents pocket-fires, feels great). Recoil: the deck
kicks 4 px on fire.

### 📡 Signal Hacker — "the terminal"
The only mostly-dark deck — amber-on-black like the panel itself. Three
**scrolling glyph lines** in a bezeled CRT window (live line glows; MOTH
fights add the flicker pulse-readout; NULL fights mark nothing — trust is
the mechanic), a **waveform strip** that visualizes the boss's current blink
cadence (a teaching aid: the waveform IS the frequency), the **SCAN** key
with a radar-arc cooldown sweep, and a small **codebook** flip-card showing
this fight's glyph→frequency mapping (reshuffled per fight — §7.4). Idle:
scanlines drift.

### 🔧 Systems Medic — "the workbench"
Cluttered-but-organized bench aesthetic. The **forge** as solder pads
(trace 1→4; Bulwark fights show the 2-pad ping fork), the **send tube** — a
pneumatic capsule that visibly *whooshes* up and off the screen (and thunks
into the Gunner's breach a beat later), the **wipe kit** scrub pad, a
**triage board** — four tiny schematic icons of the team's decks that glow
orange when acid/jam/possession lands on them (the Medic's radar), and the
**repair channel** hold-key with a progress collar. Idle: a loose wire pixel
sparks now and then.

### 🕹 Pilot deck (solo) — "the cockpit"
All four consoles miniaturized into quadrants around a central **alert
diamond** that points at whichever quadrant the current telegraph concerns.
Simplified controls (§4). Deliberately busy-but-readable — the fantasy is
"one pilot flying the whole gunship."

---

## 6. Difficulty — the SIM LEVEL knob

One five-detent knob, set in the lobby (host controls it), orthogonal to
party size (auto-normalization handles that separately).

| Detent | Name | Windows | Cadence | Hull | Enrage | Mechanics gated |
|--------|------|---------|---------|------|--------|-----------------|
| D1 | **DRILL** | ×1.5 | ×0.7 | 12 | off | no feints, no jams, no possession, no duds — pure alphabet practice |
| D2 | **FIELD** | ×1.0 | ×1.0 | 10 | 90 s | full core kit, light signatures |
| D3 | **VETERAN** | ×0.85 | ×1.15 | 10 | 90 s | full signatures, +1 mutation per fight |
| D4 | **NIGHTMARE** | ×0.7 | ×1.3 | 8 | 75 s | sloppy forges make duds, 2 mutations, adaptive drift ON (§7.3), Bulwark bash remaps |
| D5 | **SIGNAL-LOST** | ×0.6 | ×1.45 | 6 | 60 s | everything lies harder; built for 4p veterans and bragging rights |

**Assist governor** (lobby toggle, default on at D1–D2): after two
consecutive wipes on the same boss, offer +2 hull and −10% cadence for the
next attempt. Shown honestly on the intro nameplate ("ASSISTED") so it never
contaminates D5 pride.

Per-boss star ratings in the picker (Vanta ★ … NULL ★★★★) set expectations;
stars are advisory, the knob is the contract.

---

## 7. The variance engine — why no two fights repeat

Four stacked systems, cheapest first. All seeds rolled at fight start on the
ESP32 and echoed in the state blob so decks can display them.

### 7.1 Deck-draw attacks (always on)
Telegraphs are drawn from a weighted deck per boss × phase × mood — never a
script. Pity rules: no telegraph three times running; guaranteed at least
one vulnerability-window opportunity per 45 s; double-telegraphs only after
the team has cleanly handled each half separately this fight.

### 7.2 Moods (always on)
Each boss rolls a mood at fight start — announced as the nameplate epithet
("VANTA the CURIOUS") and readable in the idle animation before the first
attack (curious Vanta's eyes wander; stern Vanta's brow sits low). Moods
shift deck weights, window scalars, and signature intensity (§3 lists each
boss's three). Same boss, three different fights.

### 7.3 Adaptive drift (D4+ only)
The boss tracks the team's median response time per telegraph type and
drifts its deck weights toward what they *fumble* (cap ×1.6) — pressure
finds the weak link, which is where the shouting lives. Capped and D4+ only
so lower difficulties stay learnable.

### 7.4 Mutations (gauntlet always; Quick Fight at D3+)
1–2 rolled per fight from the pool, shown as icons on the intro nameplate:

| Mutation | Effect |
|----------|--------|
| BROWNOUT | panel dims to 40% for 3 s stretches (telegraphs persist — squint) |
| CROSSWIRE | two decks swap one control each (labels update; shouting doubles) |
| LOOSE WIRING | one control drifts (dial creeps a detent) until tapped back |
| OVERTUNED | boss cadence +15% but takes +25% damage — glass-cannon fight |
| MOTE STORM | Moth's dust mechanic guest-stars on any boss |
| ECHO | every telegraph fires twice back-to-back with −20% windows |
| CHEAP SHELLS | forge traces 1 node shorter, dud chance +15% |
| THIN ARMOR | boss +25% damage taken, hull −2 |
| STICKY ACID | acid spreads to a second deck if not wiped in 6 s |
| LONG NIGHT | enrage +30 s, boss HP +15 — the siege fight |

### 7.5 Per-fight seeds (always on)
Lane assignments shuffle, weak-side sequences reroll, the Hacker's
glyph→frequency codebook reshuffles (so "◆ means 3" is never memorized
across nights), Chorus lead order rerolls, NULL's stolen dialects reroll.

---

## 8. Session structures

### Quick Fight
Lobby → pick boss (Vanta/Moth/Chorus/Bulwark; NULL after first gauntlet
clear) → SIM LEVEL → fight → stats. The solo sim frame makes this the
practice room.

### The Gauntlet (the real evening)
Draft run: **3 of the 4 bosses in random order, then NULL.** Between fights:
hull restores +3 (cap 10) and the team drafts **1 of 3 mod cards** (per-run
upgrades):

| Mod | Effect |
|-----|--------|
| FAST FORGE | forge traces −1 node |
| CAPACITOR | overcharge cooldown −2 s |
| SPARE CELL | start each fight with a loaded shell |
| TUNED ANTENNA | scan cooldown −3 s |
| PLATING | +1 max hull |
| LUCKY FREQ | once per fight, a wrong frequency auto-corrects |
| GUST FAN | Medic's wipe also clears motes/dust in one pass |
| STEADY HAND | bash/possession can't displace your controls |

Wipe = run over, stats screen ("fell at boss 3 · VANTA the STERN · 2
mutations"). Clear = the full credits melt: every defeated boss's face
reappears on the panel and melts in sequence.

**Persistence (tiny, NVS/LittleFS):** wins per boss × difficulty, best
gauntlet depth, NULL unlock flag, lifetime stats for the stats screen.
No accounts, no meta-grind — mods are per-run only, by design.

---

## 9. Fight flow (state machine)

```
LOBBY ──ready──► INTRO (nameplate scroll: name + mood epithet + mutation
                 icons; boss wake animation; decks show cockpit headers)
      ──3·2·1──► FIGHT (20 Hz: draw telegraph → telegraph anim → response
                 window → resolve [hull hit | clean block → vuln window] →
                 idle gap; signature systems run alongside; phase gates at
                 HP thresholds fire taunt interstitials, ~2 s)
      ──hp 0───► DEATH (bespoke melt) ──► STATS (damage, blocks, saves,
                 fastest decode, "loudest role" award) ──► LOBBY / next boss
      ──hull 0 or enrage──► WIPE (grin + snap to black) ──► STATS ──► LOBBY
```

Pause states: disconnect-pause (§4), lobby-host pause. NULL's false wipe is
a scripted DEATH→reform interrupt inside FIGHT, once.

---

## 10. Content data model (how five bosses stay cheap)

Bosses are **const data tables + one anatomy program each**, not five code
forks:

```cpp
struct BossDef {
  const char* name;         // "VANTA"
  const char* title;        // "SIGNAL TITAN"
  uint8_t  anatomy;         // STANDARD / COMPOUND / TRIPLE / VISOR / NOISE
  uint16_t hp; uint8_t phases;
  uint8_t  deckW[MAX_PHASE][N_TELEGRAPH];   // draw weights
  MoodDef  moods[3];        // epithet + weight/window/signature scalars
  SigParams sig;            // feint %, riposte window, heads, noise floor…
};
```

Five anatomy programs render every face parametrically (eyes/brow/mouth/
noise/visor/bands as state). The telegraph engine, window resolution, hull,
phases, mutations, and stats are fully shared. Adding boss #6 = one table +
at most one new anatomy program. All of it fits the concept-demo code
already on the device (the §3 faces are extensions of the shipped
`raidTick` parts — eyes/brow/mouth/noise are proven on hardware).

Budget check (unchanged conclusion): state <4 KB, CPU trivial at 20 Hz,
WS ~10 KB/s. The cost is still **surface area**, not resources.

---

## 11. Milestones (revised)

- **M0 — walking skeleton** ✅ *(shipped as the concept demo: parametric
  face + 10 telegraph/personality animations on hardware, deck mockups on
  raid.html, WS-driven)*
- **M1 — one real fight** ✅ *(shipped: weighted deck-draw telegraph engine
  with pity rules — no telegraph 3× running, a beam guaranteed every 45 s;
  VANTA's three phases with taunt interstitials, moods (COLD/CURIOUS/STERN
  skew the deck and the windows), response windows / hull / vulnerability
  windows, acid + jam with medic wipe/re-sync, P3 lane fire with per-deck
  dodge and the 90 s enrage, the shell forge→send→fire pipeline and hacker
  glyph decode at 2p+ (solo keeps the §4 decode-free sim rules), lobby with
  party size, win/lose + stats debrief. Decks render from a ~10 Hz
  idempotent JSON snapshot (`raidNet` → WS). M1 notes: the four role tabs
  are functional; 2–3p deck BUNDLES and the condensed Pilot deck are M2, so
  smaller parties share tabs. Difficulty is fixed at FIELD until the M2
  knob.)* *The game exists.*
- **M2 — scaling + knob** ✅ *(shipped: the engine is now data-driven per §10
  — bosses are `FBossDef` const tables + shared anatomy programs. SIM LEVEL
  knob D1–D4 with window/cadence/hull/enrage scalars and mechanic gating
  (DRILL strips feints/afflictions/enrage/lane fire; NIGHTMARE's sloppy
  forges dud and its BASH displaces controls — see below). MOTH: feints
  that dissolve at 60% and punish a committed shield with a 3 s cooldown,
  the hacker pulse readout (REAL!/FAKE!), and DUST that hides which lane is
  targeted until wiped or shot clear. BULWARK: armored 1-damage cap, the
  ping→riposte→visor-lift BAIT loop, ping/heavy forge fork (2-node vs
  4-node trace), BASH deck shudder, and MOCKING fake lifts that never
  uncap. Deck bundles: 2p info/action, 3p
  shield/hacker/action, and the functional solo Pilot cockpit with the
  alert diamond driven by a device hint. Disconnect-pause via per-role
  heartbeats — a claimed deck silent >10 s puts the boss visibly to sleep;
  the client remembers its bundle in localStorage. Assist governor: two
  consecutive wipes at D1–D2 grant +2 hull and −10% cadence, announced as
  ASSISTED on the nameplate.*
  *Both D4 mechanics are now real rather than stubbed: **duds** come only
  from a **sloppy forge** — the Medic's deck grades its own trace (a wrong
  node, or slower than 2.5 s) and sends a lowercase key variant, so a clean
  trace is never a dud and the tax falls on panicking, not on dice; and
  **BASH** at D4 makes the device roll a displacement id that genuinely
  rearranges every deck (rocker halves swap / dial detents reverse / both)
  for the shudder. Only the layout moves — each control still sends its
  true key — so it can't be exploited or desync the device; it costs you
  the reach you'd made from muscle memory.)* *Roster depth begins.*
- **M3 — the evening.** *(in progress)*
  - **CHORUS** ✅ *(shipped: the engine now carries per-head HP pools —
    `FBossDef.heads`, with `hp` read as per-head — so damage lands on the
    head the Gunner's aim rocker (`y`/`z`) has selected, and the rocker
    steps off a corpse automatically. Phases come from KILLS rather than HP
    gates for multi-head bosses, and each death makes the survivors
    harmonize (cadence ×1.25, windows ×0.9 per kill, floored so a window
    never becomes unreadable). ROUNDS: attacks are drawn as a chain handed
    head-to-head from the lead, announced by arrows ticking across the
    lead's band, then executed back-to-back with no gap — the blocks have
    to land in sequence. Chain length is capped by party size (§4: 2/2/3/3).
    Moods set who leads: SOPRANO top, BASSO bottom, ROUND rotates every
    chain. The decks get a bar per head plus round PROGRESS only — the
    announced order stays on the panel (§0.4), so the decks never become
    the display.)*
  - **Mutations** ✅ *(shipped: all ten cards from §7.4, rolled distinct per
    fight — none below VETERAN, one at D3, two at D4 (the gauntlet will
    always roll them). Most are scalar overrides on values the engine
    already funnels through `rollGap` / `rollWin` / `bossDamage`, which is
    what makes a ten-card pool affordable: OVERTUNED, THIN ARMOR, LONG
    NIGHT, ECHO, CHEAP SHELLS, MOTE STORM. The behavioural four: ECHO
    re-arms the same telegraph once, back-to-back (afflictions excluded —
    echoing an acid landing doubles the affliction, not the reading);
    STICKY ACID creeps onto a second deck after 6 s unwiped and the wipe
    then promotes the spread; LOOSE WIRING drifts the device-owned dial a
    detent every 4 s until tapped back; BROWNOUT scales the whole frame to
    40% in 3 s stretches as a post-pass, so telegraphs render normally
    underneath and the card really is "squint". CROSSWIRE relocates the
    Shield's OVERCHARGE onto the Gunner's deck and the Gunner's FIRE onto
    the Shield's — the controls keep their meaning and their owning role,
    they just live somewhere else, so the shouting doubles.)*
  - Gauntlet structure + mod drafts, persistence — still to do.
  *Replayable.*
- **M4 — the finale + juice.** NULL (all four corruptions, dialect theft,
  false wipe), D5, adaptive drift, bespoke deaths for all, HA audio events,
  spectator page, stats polish, human balance pass — the only pass that
  counts.

### Cut lines (still out of scope)
Internet play, accounts, procedural bosses beyond the variance engine, gyro
anything, phone-side battlefield rendering. If a feature fights "the panel
is sacred," the panel wins.

---

## 12. Resolved questions from v0.1 + new open ones

Resolved in v0.2:
- **Solo:** supported via the Pilot deck / training-sim frame (decode layer
  removed — that layer IS the multiplayer, so solo replaces it with
  panel-attention). v0.1's "not supported" is reversed.
- **Comeback mechanic:** the assist governor (§6) instead of heal-on-block —
  keeps blocks pure, puts mercy in the difficulty system where it's honest.
- **Bullet-hell dodge:** stays per-deck DODGE (v0.1 default); the Medic is
  overloaded enough by design.

Open for playtest (M1–M2):
1. Chorus harmonize scalar (+25% per death) — brutal or thrilling with 2
   heads down? Needs humans.
2. Bulwark ping/riposte at 2p — is one player decoding AND blocking the
   1.8 s riposte fair, or does 2p need riposte windows ×1.2?
3. NULL inversion at solo — funny once or miserable always? May gate to 2p+.
4. Boss face identity pass: put all five anatomies on the actual panel as a
   voting reel (the panel is the only honest preview device).
