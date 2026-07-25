/* Raid 16 — M2 deck client (docs/raid16.md v0.2 §11).
   The device is authoritative: consoles send single-char keys with their
   console's ROLE as the player number, and decks render themselves from the
   ~10 Hz {"type":"raid"} snapshot. M2 adds deck bundles (2p: info/action;
   3p: shield/hacker/action; 1p: the condensed Pilot cockpit), the SIM LEVEL
   knob, MOTH's feint pulse + dust blindness, BULWARK's ping/heavy forge fork
   + riposte/visor loop + bash shudder, disconnect heartbeats, and the assist
   governor chip. Phones never mirror the telegraphs (§0.4) — each deck sees
   only what its role needs; reading the panel is the game. */

let ws = null;

/* ---------------- boss + animation strips (showcase) ---------------- */
const BOSSES = {
  V: { name: 'VANTA',   epithet: 'the signal titan',
       anims: [[0,'idle'],[1,'sweep ←'],[2,'sweep →'],[3,'beam f3'],[4,'charge'],
               [5,'acid'],[6,'jam'],[7,'enrage'],[8,'bullet hell'],[9,'melt']] },
  M: { name: 'MOTH',    epithet: 'the feintweaver',
       anims: [[0,'idle'],[1,'FEINT'],[2,'real sweep'],[3,'dust'],[4,'flurry'],
               [5,'frenzy'],[9,'death']] },
  C: { name: 'THE CHORUS', epithet: 'three-as-one',
       anims: [[0,'idle'],[1,'round'],[2,'lead change'],[3,'head down'],
               [4,'unison grin'],[9,'death']] },
  B: { name: 'BULWARK', epithet: 'the sealed door',
       anims: [[0,'idle'],[1,'riposte'],[2,'visor lift'],[3,'bash'],
               [4,'fake lift'],[9,'death']] },
  N: { name: 'NULL',    epithet: 'the static king',
       anims: [[0,'idle'],[1,'cohere+beam'],[2,'possession'],[3,'inversion'],
               [4,'false wipe'],[9,'death']] },
};
let curBoss = 'V';

/* ---------------- device state ---------------- */
const $ = (id) => document.getElementById(id);
const GLYPHS = ['◇', '◆', '▲', '▽'];
const ROLE_KEYS = ['shl', 'gun', 'hck', 'med'];
const ROLE_NAMES = ['shield', 'gunner', 'hacker', 'medic'];
const DIFF_ORDER = ['DRILL', 'FIELD', 'VETERAN', 'NIGHTMARE'];

/* Deck bundles (§4/§5): consoles never disappear, they consolidate. */
const BUNDLES = {
  1: [{ label: '🚀 pilot',      panes: ['pil'],        roles: [0, 1, 2, 3] }],
  2: [{ label: '🛡📡 info',     panes: ['shl', 'hck'], roles: [0, 2] },
      { label: '🔫🔧 action',   panes: ['gun', 'med'], roles: [1, 3] }],
  3: [{ label: '🛡 shield',     panes: ['shl'],        roles: [0] },
      { label: '📡 hacker',     panes: ['hck'],        roles: [2] },
      { label: '🔫🔧 action',   panes: ['gun', 'med'], roles: [1, 3] }],
  4: [{ label: '🛡 shield',     panes: ['shl'],        roles: [0] },
      { label: '🔫 gunner',     panes: ['gun'],        roles: [1] },
      { label: '📡 hacker',     panes: ['hck'],        roles: [2] },
      { label: '🔧 medic',      panes: ['med'],        roles: [3] }],
};

let snap = null;              // latest device snapshot (null = idle/showcase)
let lastSnapAt = 0;
let lastEv = -1;
let party = 4;                // last known party size (drives the tab bundles)
let bundleIdx = +(localStorage.getItem('raid.bundle') || 0);
let myRoles = [0];            // roles of the open bundle (dodge/alert/heartbeat scope)

const st = () => (snap ? snap.st : 'idle');
const inFight = () => st() === 'fight';
const isBulwark = () => snap && snap.boss === 'BULWARK';

function connectWS() {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  ws = new WebSocket(`${proto}://${location.host}/ws`);
  ws.binaryType = 'arraybuffer';
  ws.onopen = () => {
    setConn(true);
    send({ cmd: 'game', name: 'raid' });
  };
  ws.onmessage = (e) => {
    if (typeof e.data !== 'string') return;
    let m; try { m = JSON.parse(e.data); } catch { return; }
    if (m.type === 'raid') { lastSnapAt = Date.now(); handleSnap(m); }
  };
  ws.onclose = () => { setConn(false); setTimeout(connectWS, 2000); };
}
function send(o) { if (ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(o)); }
/* every console sends with ITS role — that's how acid/jam/lane targeting and
   the disconnect heartbeats stay honest across bundles */
function roleKey(k, role) { send({ cmd: 'input', p: role, k: String(k), s: 1 }); }
function key(k) { roleKey(k, myRoles[0]); }
function setConn(on) {
  const el = $('raid-conn');
  el.textContent = on ? 'online' : 'offline';
  el.className = `chip ${on ? 'online' : 'offline'}`;
}

/* heartbeats: prove each of my roles alive (device pauses a silent deck) */
setInterval(() => { if (snap) myRoles.forEach(r => roleKey('H', r)); }, 2000);

/* the device goes silent in showcase mode — treat a stale snapshot as idle */
setInterval(() => {
  if (snap && Date.now() - lastSnapAt > 1200) { snap = null; renderFlow(); }
}, 400);

function buildBossStrip() {
  const strip = $('boss-strip');
  strip.innerHTML = '';
  for (const [k, b] of Object.entries(BOSSES)) {
    const btn = document.createElement('button');
    btn.className = 'segmented-btn' + (k === curBoss ? ' active' : '');
    btn.textContent = b.name.toLowerCase();
    btn.onpointerdown = (e) => { e.preventDefault(); selectBoss(k); };
    strip.appendChild(btn);
  }
}
function buildAnimGrid() {
  const grid = $('anim-grid');
  grid.innerHTML = '';
  BOSSES[curBoss].anims.forEach(([d, label], i) => {
    const btn = document.createElement('button');
    btn.className = 'segmented-btn' + (i === 0 ? ' active' : '');
    btn.textContent = label;
    btn.onpointerdown = (e) => {
      e.preventDefault();
      grid.querySelectorAll('.segmented-btn').forEach(x => x.classList.remove('active'));
      btn.classList.add('active');
      key(d);
    };
    grid.appendChild(btn);
  });
}
function selectBoss(k) {
  curBoss = k;
  key(k);
  buildBossStrip(); buildAnimGrid();
  if (st() === 'idle') {
    $('np-name').textContent = BOSSES[k].name;
    $('np-epithet').textContent = BOSSES[k].epithet;
  }
}

/* ---------------- deck tabs: bundles per party size ---------------- */
function rebuildTabs() {
  const defs = BUNDLES[party] || BUNDLES[4];
  if (bundleIdx >= defs.length) bundleIdx = 0;
  const bar = $('deck-tabs');
  bar.innerHTML = '';
  defs.forEach((b, i) => {
    const t = document.createElement('button');
    t.className = 'tab' + (i === bundleIdx ? ' active' : '');
    t.textContent = b.label;
    t.onclick = () => { bundleIdx = i; localStorage.setItem('raid.bundle', i); applyBundle(); };
    bar.appendChild(t);
  });
  applyBundle();
}
function applyBundle() {
  const defs = BUNDLES[party] || BUNDLES[4];
  const b = defs[Math.min(bundleIdx, defs.length - 1)];
  myRoles = b.roles;
  [...$('deck-tabs').children].forEach((t, i) => t.classList.toggle('active', i === bundleIdx));
  document.querySelectorAll('.deck').forEach(d => d.classList.remove('active'));
  b.panes.forEach(p => $('deck-' + p).classList.add('active'));
  if (snap) handleSnap(snap);                 // re-filter alerts for the new roles
}

/* ---------------- fight flow card ---------------- */
$('fight-start').onclick = () => key('G');
$('fight-stop').onclick  = () => key('Q');
$('stats-rematch').onclick = () => key('G');
$('stats-exit').onclick    = () => key('Q');
document.querySelectorAll('#party-strip .segmented-btn').forEach(b =>
  b.onpointerdown = (e) => { e.preventDefault(); if (st() === 'lobby') key(b.dataset.p); });
document.querySelectorAll('#fboss-strip .segmented-btn').forEach(b =>
  b.onpointerdown = (e) => { e.preventDefault(); if (st() === 'lobby') key(b.dataset.b); });
$('diff-cycle').onclick = () => { if (st() === 'lobby') key('D'); };

const ST_LABEL = { idle: 'standby', lobby: 'lobby', intro: 'incoming…',
                   fight: 'FIGHT', win: 'victory', lose: 'wiped', stats: 'debrief' };

function renderFlow() {
  const s = st();
  $('fight-st').textContent = ST_LABEL[s] || s;
  // Hidden from the lobby on: the anim strip's digit keys are party-size /
  // freq-dial keys inside the fight flow, and the panel is no longer showcasing.
  $('showcase-card').style.display = s === 'idle' ? '' : 'none';
  $('lobby-ui').hidden = s !== 'lobby';
  $('fight-start').hidden = !(s === 'idle' || s === 'lobby');
  $('fight-start').textContent = s === 'idle' ? 'ENTER LOBBY' : 'START FIGHT';
  $('fight-stop').hidden = s === 'idle';
  $('fight-stop').textContent = s === 'lobby' ? 'back' : 'abandon';

  const statsUp = (s === 'win' || s === 'lose' || s === 'stats') && snap && snap.stats;
  $('stats-card').hidden = !statsUp;
  if (statsUp) renderStats(snap.stats);

  if (s === 'idle') {
    $('np-name').textContent = BOSSES[curBoss].name;
    $('np-epithet').textContent = BOSSES[curBoss].epithet;
    $('hp-fill').style.width = '100%';
    $('enrage-tag').hidden = true; $('vuln-tag').hidden = true;
    $('visor-tag').hidden = true;  $('dust-tag').hidden = true;
    $('deck-alert').hidden = true; $('dodge').hidden = true;
    $('resync-rig').hidden = true; $('breach-hint').hidden = true;
    $('shl-cd-alert').hidden = true; $('fire-ping').hidden = true;
    $('p-ping').hidden = true;
    $('mock-load').hidden = false;
    $('cockpit').classList.remove('bashing');
    paintHull(10, 10); paintPhase(1);
  }
}

function renderStats(t) {
  const boss = snap ? snap.boss : 'VANTA';
  $('stats-res').textContent = t.res === 'win'
    ? `${boss.toLowerCase()} down — victory` : `wiped — ${boss.toLowerCase()} grins`;
  const rows = [
    ['fight length', t.sec + 's'], ['hull lost', t.hullLost],
    ['clean blocks', t.blk], ['misses', t.miss],
    ['damage dealt', t.dmg], ['window damage', t.vdmg],
    ['shots fired', t.shots], ['charge interrupts', t.itr],
    ['acid wiped', t.wip], ['jams re-synced', t.fix],
  ];
  $('stats-grid').innerHTML = rows.map(([k, v]) =>
    `<div class="srow"><span>${k}</span><b>${v}</b></div>`).join('');
}

/* ---------------- cockpit header ---------------- */
let hullMaxShown = 10;
function paintHull(h, max) {
  const wrap = $('hull-leds');
  if (max !== hullMaxShown || wrap.children.length !== max) {
    hullMaxShown = max;
    wrap.innerHTML = '';
    for (let i = 0; i < max; i++) wrap.appendChild(document.createElement('span'));
  }
  [...wrap.children].forEach((d, i) => d.className = 'hled' + (i < h ? '' : ' off'));
}
function paintPhase(ph) {
  [...$('phase-pips').children].forEach((p, i) => p.className = 'pip' + (i < ph ? ' on' : ''));
}
paintHull(10, 10);
$('dodge').onpointerdown = (e) => {
  e.preventDefault();
  if (!snap) return;
  if (snap.lane >= 0) { roleKey('X', snap.lane); return; }
  // Dust: the device withholds WHICH lane. Fire a dodge for each role this
  // deck owns — the device only honours the real target, so the team still
  // has to read the buried stage markers to know who acts.
  myRoles.forEach(r => roleKey('X', r));
};

function cockpitFx(evn) {
  const good = ['block', 'vuln', 'vulnhit', 'interrupt', 'dodge', 'wiped', 'resync',
                'win', 'visor', 'feint', 'dustclear', 'dustshot'];
  const bad  = ['bosshit', 'lanehit', 'rsyfail', 'wipe', 'acid', 'jam', 'dust',
                'feintpunish', 'baited', 'dud', 'bash'];
  const cls = good.includes(evn) ? 'fx-good' : bad.includes(evn) ? 'fx-bad' : null;
  if (!cls) return;
  const c = $('cockpit');
  c.classList.remove('fx-good', 'fx-bad'); void c.offsetWidth;
  c.classList.add(cls);
  setTimeout(() => c.classList.remove(cls), 350);
}

/* ---------------- snapshot → UI ---------------- */
function handleSnap(s) {
  snap = s;
  if (s.party !== party) { party = s.party; rebuildTabs(); }
  renderFlow();

  const fightish = s.st === 'intro' || s.st === 'fight';
  if (fightish || s.st === 'win' || s.st === 'lose' || s.st === 'stats') {
    $('np-name').textContent = s.boss;
    $('np-epithet').textContent =
      `mood: ${s.mood.toLowerCase()} · P${s.ph} · ${s.diff.toLowerCase()}` +
      (s.assist ? ' · assisted' : '');
  } else if (s.st === 'lobby') {
    $('np-name').textContent = s.boss;
    $('np-epithet').textContent = `${s.diff.toLowerCase()} · party ${s.party}` +
      (s.assist ? ' · assist armed' : '');
  }
  $('hp-fill').style.width =
    Math.max(0, Math.min(100, 100 * s.hp / (s.hpMax || 100))) + '%';
  paintHull(s.hull, s.hullMax || 10); paintPhase(s.ph);

  $('enrage-tag').hidden = !(s.enrage >= 0 && fightish);
  $('enrage-tag').textContent = `enrage ${s.enrage}s`;
  $('vuln-tag').hidden = !(s.vulnMs > 0);
  $('visor-tag').hidden = !(s.visor > 0);
  $('dust-tag').hidden = !(s.dust && inFight());
  $('cockpit').classList.toggle('bashing', s.bash > 0);

  /* lobby pickers */
  document.querySelectorAll('#party-strip .segmented-btn').forEach(b =>
    b.classList.toggle('active', +b.dataset.p === s.party));
  document.querySelectorAll('#fboss-strip .segmented-btn').forEach(b =>
    b.classList.toggle('active', BOSSES[b.dataset.b].name === s.boss));
  const dIdx = Math.max(0, DIFF_ORDER.indexOf(s.diff));
  $('diff-cycle').textContent = `D${dIdx + 1} · ${s.diff}`;
  $('assist-chip').hidden = !s.assist;

  /* my bundle's alerts */
  const alerts = [];
  if (s.paused >= 0) alerts.push(`DECK ${ROLE_NAMES[s.paused].toUpperCase()} LOST — boss asleep`);
  if (myRoles.includes(s.acid)) alerts.push('ACID ON YOUR DECK — shout for the medic!');
  if (myRoles.includes(s.jam))  alerts.push('JAMMED — medic re-sync!');
  $('deck-alert').hidden = alerts.length === 0;
  $('deck-alert').textContent = alerts.join('  ·  ');
  // Normal: only the targeted deck sees DODGE. Under dust the lane is hidden,
  // so every deck gets the button and the panel decides who's actually right.
  const dodgeHidden = s.dust ? !s.laneUp : !(s.lane >= 0 && myRoles.includes(s.lane));
  $('dodge').hidden = dodgeHidden;
  $('dodge').textContent = s.dust ? 'DODGE — WHOSE LANE?' : 'DODGE!';
  $('shl-cd-alert').hidden = !(s.shlCd > 0 && myRoles.includes(0));

  /* gunner: breach + crank reconcile (the device is the truth) */
  if (isBulwark() && s.party >= 2) {
    $('shell-ping').classList.toggle('loaded', s.ping >= 1);
    $('shell-heavy').classList.toggle('loaded', s.heavy >= 1);
  } else {
    $('shell-ping').classList.toggle('loaded', s.party >= 2 ? s.shells >= 1 : s.crank >= 100);
    $('shell-heavy').classList.toggle('loaded', s.party >= 2 && s.shells >= 2);
  }
  $('mock-load').hidden = s.st !== 'idle';
  $('breach-hint').hidden = !(fightish && s.party >= 2);
  $('fire-ping').hidden = !(inFight() && isBulwark());
  $('p-ping').hidden = !(inFight() && isBulwark());
  if (!crankDragging && inFight()) {
    crankPct = s.crank; crankSent = Math.floor(s.crank / 10);
    paintCrank();
  }
  $('p-crank-pct').textContent = (inFight() ? s.crank : Math.round(crankPct)) + '%';

  /* hacker: live glyph + feint pulse + codebook + resync relay */
  paintHackerCrt(s);
  paintCodebook(s.code);

  /* medic: triage + resync pad + wipe slimes + forge labels */
  ROLE_KEYS.forEach((r, i) =>
    $('tri-' + r).classList.toggle('alert', s.acid === i || s.jam === i));
  $('resync-rig').hidden = !(s.jam >= 0 && inFight());
  paintForge();
  syncSlimes(s);

  /* solo pilot: the alert diamond points at what needs you */
  paintPilot(s);

  if (s.ev !== lastEv) { lastEv = s.ev; cockpitFx(s.evn); }
}

/* press-latch helper */
function holdFx(el, onDown, onUp) {
  el.addEventListener('pointerdown', (e) => { e.preventDefault(); el.classList.add('pressed'); onDown && onDown(); });
  ['pointerup', 'pointercancel', 'pointerleave'].forEach(ev =>
    el.addEventListener(ev, () => { el.classList.remove('pressed'); onUp && onUp(); }));
}

/* ============ SHIELD — breaker panel (role 0) ============ */
const seesaw = $('seesaw');
const shlL = $('shl-l'), shlR = $('shl-r');
function tilt(side) {
  seesaw.classList.toggle('tilt-l', side === 'L');
  seesaw.classList.toggle('tilt-r', side === 'R');
  shlL.classList.toggle('pressed', side === 'L');
  shlR.classList.toggle('pressed', side === 'R');
  if (inFight()) roleKey(side, 0);
}
shlL.onpointerdown = (e) => { e.preventDefault(); tilt('L'); };
shlR.onpointerdown = (e) => { e.preventDefault(); tilt('R'); };

const knob = $('freq-knob');
const stem = $('knob-stem');
const DETENT_ANG = [-60, -20, 20, 60];
let freq = 1;
function setFreq(f) {
  freq = f;
  stem.style.transform = `rotate(${DETENT_ANG[f - 1] + 180}deg)`;
  document.querySelectorAll('#knob-detents span').forEach(s =>
    s.classList.toggle('on', +s.dataset.f === f));
  if (inFight()) roleKey(f, 0);
}
knob.addEventListener('pointerdown', (e) => { e.preventDefault(); knob.setPointerCapture(e.pointerId); });
knob.addEventListener('pointermove', (e) => {
  if (e.buttons === 0) return;
  const r = knob.getBoundingClientRect();
  const a = Math.atan2(e.clientY - (r.top + r.height / 2), e.clientX - (r.left + r.width / 2)) * 180 / Math.PI;
  const rel = a - 90;
  let best = 1, bd = 1e9;
  DETENT_ANG.forEach((d, i) => { const diff = Math.abs(rel - d); if (diff < bd) { bd = diff; best = i + 1; } });
  if (best !== freq) setFreq(best);
});
document.querySelectorAll('#knob-detents span').forEach(s =>
  s.addEventListener('pointerdown', (e) => { e.preventDefault(); setFreq(+s.dataset.f); }));
setFreq(3);

const overBtn = $('shl-over');
const overFill = $('over-fill');
const capFill = $('cap-fill');
let overT = null, overPct = 0;
holdFx(overBtn,
  () => { overT = setInterval(() => {
      overPct = Math.min(100, overPct + 7);
      overFill.style.width = overPct + '%';
      if (overPct >= 100 && inFight() && !overBtn.dataset.armed) {
        overBtn.dataset.armed = '1';
        roleKey('O', 0);
      }
    }, 60); },
  () => {
    clearInterval(overT);
    if (overPct >= 100) {
      capFill.style.transition = 'none'; capFill.style.width = '0%';
      requestAnimationFrame(() => { capFill.style.transition = 'width 6s linear'; capFill.style.width = '100%'; });
    }
    overPct = 0; overFill.style.width = '0%';
    delete overBtn.dataset.armed;
  });

/* ============ GUNNER — artillery station (role 1) ============ */
const crank = $('crank');
let crankPct = 0, crankSent = 0, lastAng = null, spinVel = 0, crankDragging = false;
function paintCrank() {
  crank.style.background = `conic-gradient(var(--accent) ${crankPct * 3.6}deg, var(--surface2) 0deg)`;
  crank.querySelector('span').innerHTML = `crank<br>${Math.round(crankPct)}%`;
}
crank.addEventListener('pointerdown', (e) => { e.preventDefault(); lastAng = null; crankDragging = true; crank.setPointerCapture(e.pointerId); });
['pointerup', 'pointercancel'].forEach(ev =>
  crank.addEventListener(ev, () => { crankDragging = false; }));
crank.addEventListener('pointermove', (e) => {
  if (e.buttons === 0) return;
  const r = crank.getBoundingClientRect();
  const a = Math.atan2(e.clientY - (r.top + r.height / 2), e.clientX - (r.left + r.width / 2));
  if (lastAng !== null) {
    let d = a - lastAng;
    if (d > Math.PI) d -= 2 * Math.PI; if (d < -Math.PI) d += 2 * Math.PI;
    spinVel = Math.abs(d) * 12;
    bumpCrank(spinVel);
  }
  lastAng = a;
});
function bumpCrank(amt) {
  crankPct = Math.min(100, crankPct + amt);
  paintCrank();
  if (inFight()) {
    const steps = Math.floor(crankPct / 10);
    while (crankSent < steps) { crankSent++; roleKey('K', 1); }
  }
}
(function momentum() {
  if (spinVel > 0.3) { spinVel *= 0.92; bumpCrank(spinVel * 0.4); }
  requestAnimationFrame(momentum);
})();

const shellPing = $('shell-ping');
const shellHeavy = $('shell-heavy');
$('mock-load').onclick = () => {
  if (!shellPing.classList.contains('loaded')) shellPing.classList.add('loaded');
  else if (!shellHeavy.classList.contains('loaded')) shellHeavy.classList.add('loaded');
  else { shellPing.classList.remove('loaded'); shellHeavy.classList.remove('loaded'); }
};

const gunL = $('gun-l'), gunR = $('gun-r');
const lamps = ['lampL', 'lampM', 'lampR'].map(id => $(id));
let aim = 1;
function setAim(a) {                          // head targeting arrives with CHORUS (M3)
  aim = a;
  lamps.forEach((l, i) => l.classList.toggle('on', i === a));
  gunL.classList.toggle('pressed', a === 0);
  gunR.classList.toggle('pressed', a === 2);
}
gunL.onpointerdown = (e) => { e.preventDefault(); setAim(Math.max(0, aim - 1)); };
gunR.onpointerdown = (e) => { e.preventDefault(); setAim(Math.min(2, aim + 1)); };

const fireCover = $('fire-cover');
const fireBtn = $('fire');
fireCover.addEventListener('pointerdown', (e) => { e.preventDefault(); fireCover.classList.add('open'); });
holdFx(fireBtn, () => {
  const deck = $('deck-gun');
  deck.classList.remove('kick'); void deck.offsetWidth; deck.classList.add('kick');
  if (inFight()) roleKey('F', 1);
  else if (crankPct >= 100) {
    (shellHeavy.classList.contains('loaded') ? shellHeavy : shellPing).classList.remove('loaded');
    crankPct = 0; crankSent = 0; paintCrank();
  }
  setTimeout(() => fireCover.classList.remove('open'), 900);
});
$('fire-ping').onpointerdown = (e) => { e.preventDefault(); if (inFight()) roleKey('P', 1); };

/* ============ HACKER — terminal (role 2) ============ */
const crtLines = [0, 1, 2].map(i => $('gl' + i));
let idleOffs = [0, 3, 6];
const IDLE_GLYPHS = '◇◆▲▽□■○●';

/* Live telemetry. Beam (2p+): the live line locks to the device's glyph — and
   MOTH's pulse readout makes it FLICKER when the telegraph is a feint (§3.2):
   the hacker's job is shouting REAL!/FAKE!. Jam: the resync relay. */
function paintHackerCrt(s) {
  const beam = s.glyph >= 0;
  if (beam) {
    crtLines.forEach((el, i) => {
      el.classList.toggle('live', i === 1);
      el.classList.toggle('flicker', i === 1 && s.pulse === 1);
      el.textContent = i === 1 ? GLYPHS[s.glyph].repeat(6)
                               : IDLE_GLYPHS.split('').sort(() => Math.random() - 0.5).slice(0, 6).join('');
    });
  } else if (s.pulse === 1) {                 // feint on a sweep: no glyph, pure pulse
    crtLines[0].textContent = 'PULSE UNSTABLE';
    crtLines[1].textContent = '≋ FAKE? ≋';
    crtLines[2].textContent = 'CALL IT';
    crtLines.forEach((el, i) => { el.classList.toggle('live', i === 1);
                                  el.classList.toggle('flicker', i === 1); });
  } else if (s.jam >= 0) {
    crtLines[0].textContent = 'DECK ' + ROLE_NAMES[s.jam].toUpperCase();
    crtLines[1].textContent = 'RESYNC KEY → ' + (s.rsy + 1);
    crtLines[2].textContent = 'RELAY TO MEDIC';
    crtLines.forEach((el, i) => { el.classList.toggle('live', i === 1); el.classList.remove('flicker'); });
  }
}
setInterval(() => {
  if (snap && (snap.glyph >= 0 || snap.jam >= 0 || snap.pulse === 1)) return;
  crtLines.forEach((el, i) => {
    idleOffs[i] = (idleOffs[i] + 1) % IDLE_GLYPHS.length;
    let s = '';
    for (let k = 0; k < 6; k++) s += IDLE_GLYPHS[(idleOffs[i] + k * 2 + i) % IDLE_GLYPHS.length];
    el.textContent = s;
  });
}, 450);
setInterval(() => {
  if (snap && (snap.glyph >= 0 || snap.jam >= 0 || snap.pulse === 1)) return;
  const live = Math.floor(Math.random() * 3);
  crtLines.forEach((el, i) => {
    el.classList.toggle('live', i === live);
    el.classList.remove('flicker');
  });
}, 3500);

const wf = $('waveform');
(function drawWave() {
  const ctx = wf.getContext('2d');
  const t = performance.now() / 1000;
  ctx.fillStyle = '#0d0d0d'; ctx.fillRect(0, 0, wf.width, wf.height);
  ctx.strokeStyle = '#f5a623'; ctx.lineWidth = 2; ctx.beginPath();
  const period = 60 / (freq * 1.6 + 1);
  for (let x = 0; x < wf.width; x++) {
    const phase = ((x + t * 40) % period) / period;
    const y = phase < 0.5 ? 6 : wf.height - 6;
    x === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
  }
  ctx.stroke();
  requestAnimationFrame(drawWave);
})();

const scanBtn = $('scan'), scanArc = $('scan-arc');
let scanning = false;
holdFx(scanBtn, () => {
  if (scanning) return;
  scanning = true; scanBtn.disabled = true;
  const t0 = performance.now();
  (function arc() {
    const p = Math.min(1, (performance.now() - t0) / 8000);
    scanArc.style.background = `conic-gradient(var(--accent) ${p * 360}deg, transparent 0deg)`;
    if (p < 1) requestAnimationFrame(arc);
    else { scanning = false; scanBtn.disabled = false; scanArc.style.background = ''; }
  })();
});

let codeShown = '';
function paintCodebook(code) {
  const sig = code ? code.join('') : '';
  if (sig === codeShown) return;
  codeShown = sig;
  const cb = $('codebook');
  cb.innerHTML = '';
  (code || [1, 2, 3, 4]).forEach((f, i) => {
    const s = document.createElement('span');
    s.innerHTML = `${GLYPHS[i]}&thinsp;=&thinsp;<b>${f}</b>`;
    cb.appendChild(s);
  });
}
paintCodebook(null);

/* ============ MEDIC — workbench (role 3) ============ */
/* BULWARK's forge fork (§3.4): a 2-node trace is a PING, the full 4-node
   trace is a HEAVY. Everyone else forges the full trace for a generic shell. */
let forgeNext = 1;
const sendBtn = $('send-shell');
function paintForge() {
  const fork = isBulwark() && inFight();
  if (forgeNext === 3 && fork) { sendBtn.disabled = false; sendBtn.textContent = 'send PING'; }
  else if (forgeNext === 5)    { sendBtn.disabled = false; sendBtn.textContent = fork ? 'send HEAVY' : 'send shell'; }
  else { sendBtn.disabled = forgeNext !== 3 || !fork; if (sendBtn.disabled) sendBtn.textContent = 'send to gunner'; }
}
document.querySelectorAll('.node:not(.rsy)').forEach(n => {
  n.onpointerdown = (e) => {
    e.preventDefault();
    if (+n.dataset.n === forgeNext) { n.classList.add('lit'); forgeNext++; }
    else { document.querySelectorAll('.node:not(.rsy)').forEach(x => x.classList.remove('lit')); forgeNext = 1; }
    paintForge();
  };
});
sendBtn.onclick = () => {
  const ping = forgeNext === 3;                // fork: 2 nodes = ping, 4 = heavy
  document.querySelectorAll('.node:not(.rsy)').forEach(x => x.classList.remove('lit'));
  forgeNext = 1; sendBtn.disabled = true; sendBtn.textContent = 'send to gunner';
  const cap = $('capsule');
  cap.classList.remove('whoosh'); void cap.offsetWidth; cap.classList.add('whoosh');
  if (inFight()) roleKey(ping && isBulwark() ? 'U' : 'T', 3);
  setTimeout(() => {
    if (!inFight()) shellPing.classList.add('loaded');
    cap.classList.remove('whoosh');
  }, 600);
};

document.querySelectorAll('.node.rsy').forEach(n =>
  n.onpointerdown = (e) => { e.preventDefault(); roleKey(String.fromCharCode(97 + +n.dataset.r), 3); });

setInterval(() => {
  if (snap) return;
  const icons = ROLE_KEYS.map(r => $('tri-' + r));
  icons.forEach(i => i.classList.remove('alert'));
  if (Math.random() < 0.6) icons[Math.floor(Math.random() * 4)].classList.add('alert');
}, 4000);

/* wipe pad: acid slimes first; MOTH's dust also lands here ("gust" = the same
   scrub). Each stroke that lands sends 'W'; the device counts to clean. */
const pad = $('wipe-pad');
let lastWipeSent = 0;
function spawnSlime(dusty) {
  if (pad.querySelectorAll('.slime').length >= 4) return;
  const s = document.createElement('div');
  s.className = 'slime';
  if (dusty) s.style.background = '#8a8a7a';
  s.style.left = (5 + Math.random() * 70) + '%';
  s.style.top = (8 + Math.random() * 55) + '%';
  s.dataset.hp = 6;
  pad.appendChild(s);
}
function syncSlimes(s) {
  const busy = (s.acid >= 0 || s.dust) && inFight();
  const have = pad.querySelectorAll('.slime').length;
  if (busy && have < 3) spawnSlime(s.acid < 0);
  if (!busy && snap && have) pad.querySelectorAll('.slime').forEach(x => x.remove());
}
setInterval(() => { if (!snap) spawnSlime(false); }, 2600);
spawnSlime(false); spawnSlime(false);
pad.addEventListener('pointermove', (e) => {
  if (e.buttons === 0) return;
  pad.querySelectorAll('.slime').forEach(sl => {
    const r = sl.getBoundingClientRect();
    if (e.clientX >= r.left - 6 && e.clientX <= r.right + 6 &&
        e.clientY >= r.top - 6 && e.clientY <= r.bottom + 6) {
      if (--sl.dataset.hp <= 0) sl.remove();
      else sl.style.opacity = sl.dataset.hp / 6;
      if (inFight() && snap && (snap.acid >= 0 || snap.dust) &&
          Date.now() - lastWipeSent > 120) {
        lastWipeSent = Date.now();
        roleKey('W', 3);
      }
    }
  });
});

const repBtn = $('repair'), collar = $('rep-collar');
let repT = null, rep = 0;
holdFx(repBtn,
  () => { repT = setInterval(() => {
      rep = Math.min(100, rep + 4);
      collar.style.background = `conic-gradient(var(--accent) ${rep * 3.6}deg, var(--surface) 0deg)`;
    }, 200); },
  () => { clearInterval(repT); if (rep >= 100) rep = 0;
          if (!rep) collar.style.background = ''; });

/* ============ PILOT — the solo cockpit (party 1: all four roles) ============ */
document.querySelectorAll('.p-key').forEach(b =>
  b.addEventListener('pointerdown', (e) => {
    e.preventDefault();
    b.classList.add('active'); setTimeout(() => b.classList.remove('active'), 200);
    const k = b.dataset.k;
    let role = 0;
    if (b.dataset.r === '3') role = 3;
    if (b.dataset.r === '-1') role = snap && snap.lane >= 0 ? snap.lane : 0;
    if (!inFight()) return;
    roleKey(k, role);
    if (k >= '1' && k <= '4')
      document.querySelectorAll('#p-dial .p-key').forEach(x =>
        x.classList.toggle('active', x === b));
  }));
let pCrankT = null;
holdFx($('p-crank'),
  () => { if (inFight()) { roleKey('K', 1);
          pCrankT = setInterval(() => roleKey('K', 1), 140); } },
  () => clearInterval(pCrankT));
$('p-fire').onpointerdown = (e) => { e.preventDefault(); if (inFight()) roleKey('F', 1); };
$('p-ping').onpointerdown = (e) => { e.preventDefault(); if (inFight()) roleKey('P', 1); };

const quads = ['q-shl', 'q-gun', 'q-hck', 'q-med'].map(id => $(id));
const diamond = $('alert-diamond');
const DIA_POS = [
  'translate(-140%,-140%) rotate(45deg)', 'translate(40%,-140%) rotate(45deg)',
  'translate(-140%,40%) rotate(45deg)',   'translate(40%,40%) rotate(45deg)'];
/* the diamond follows the device's hint: 1/2 = shield quad, 3 = guns, 4 = systems */
function paintPilot(s) {
  const q = inFight() ? ({ 1: 0, 2: 0, 3: 1, 4: 3 })[s.hint] : undefined;
  quads.forEach((el, i) => el.classList.toggle('alert', i === q));
  if (q !== undefined) diamond.style.transform = DIA_POS[q];
  $('mini-glyph').textContent =
    s.pulse === 1 ? '≋ FAKE? ≋' :
    s.hint === 2  ? 'COUNT THE BLINKS' :
    s.hint === 1  ? 'ROCKER — WHICH SIDE?' :
    s.hint === 3  ? 'WINDOW — FIRE!' : '· · ·';
}
setInterval(() => {                            // idle demo only
  if (snap) return;
  const q = Math.floor(Math.random() * 4);
  quads.forEach((el, i) => el.classList.toggle('alert', i === q));
  diamond.style.transform = DIA_POS[q];
  $('mini-glyph').textContent =
    IDLE_GLYPHS.split('').sort(() => Math.random() - 0.5).slice(0, 3).join('');
}, 3000);

/* ---------------- boot ---------------- */
buildBossStrip();
buildAnimGrid();
rebuildTabs();
renderFlow();
connectWS();
