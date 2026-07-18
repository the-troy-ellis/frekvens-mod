/* Raid 16 — deck review build (docs/raid16.md v0.2).
   The boss + animation strips are REAL (they drive the physical panel over
   WS). The five decks are tactile visual mockups for review: every control
   gives press/drag feedback locally, but nothing is wired to game logic —
   there is no game engine yet (that's M1). */

let ws = null;

/* ---------------- boss + animation strips (REAL) ---------------- */
const BOSSES = {
  V: { name: 'VANTA',   epithet: 'the signal titan · mood: cold',
       anims: [[0,'idle'],[1,'sweep ←'],[2,'sweep →'],[3,'beam f3'],[4,'charge'],
               [5,'acid'],[6,'jam'],[7,'enrage'],[8,'bullet hell'],[9,'melt']] },
  M: { name: 'MOTH',    epithet: 'the feintweaver · mood: skittish',
       anims: [[0,'idle'],[1,'FEINT'],[2,'real sweep'],[3,'dust'],[4,'flurry'],
               [5,'frenzy'],[9,'death']] },
  C: { name: 'THE CHORUS', epithet: 'three-as-one · mood: round',
       anims: [[0,'idle'],[1,'round'],[2,'lead change'],[3,'head down'],
               [4,'unison grin'],[9,'death']] },
  B: { name: 'BULWARK', epithet: 'the sealed door · mood: patient',
       anims: [[0,'idle'],[1,'riposte'],[2,'visor lift'],[3,'bash'],
               [4,'fake lift'],[9,'death']] },
  N: { name: 'NULL',    epithet: 'the static king · no mood — it samples',
       anims: [[0,'idle'],[1,'cohere+beam'],[2,'possession'],[3,'inversion'],
               [4,'false wipe'],[9,'death']] },
};
let curBoss = 'V';

function connectWS() {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  ws = new WebSocket(`${proto}://${location.host}/ws`);
  ws.binaryType = 'arraybuffer';
  ws.onopen = () => {
    setConn(true);
    send({ cmd: 'game', name: 'raid' });
  };
  ws.onclose = () => { setConn(false); setTimeout(connectWS, 2000); };
}
function send(o) { if (ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(o)); }
function key(k) { send({ cmd: 'input', p: 0, k: String(k), s: 1 }); }
function setConn(on) {
  const el = document.getElementById('raid-conn');
  el.textContent = on ? 'online' : 'offline';
  el.className = `chip ${on ? 'online' : 'offline'}`;
}

function buildBossStrip() {
  const strip = document.getElementById('boss-strip');
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
  const grid = document.getElementById('anim-grid');
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
  key(k);                                   // V/M/C/B/N selects on the device
  buildBossStrip(); buildAnimGrid();
  document.getElementById('np-name').textContent = BOSSES[k].name;
  document.getElementById('np-epithet').textContent = BOSSES[k].epithet;
}

/* ---------------- fight mode (M1 slice — VANTA, solo-sim) ----------------
   When ON, the Shield rocker/knob/overcharge and Gunner crank/fire send real
   fight keys (L/R, 1-4, O, K, F). The device runs the whole fight; the panel
   is the display. When OFF, those same controls are inert mockups again. */
let fightOn = false;
document.getElementById('fight-start').onclick = () => {
  fightOn = true;
  key('G');
  crankPct = 0; crankSent = 0; bumpCrank(0);    // fresh flywheel each fight
  document.getElementById('fight-hint').style.color = 'var(--accent2)';
};
document.getElementById('fight-stop').onclick = () => {
  fightOn = false;
  key('Q');
  document.getElementById('fight-hint').style.color = '';
};

/* ---------------- cockpit header ---------------- */
(function hullLeds() {
  const wrap = document.getElementById('hull-leds');
  for (let i = 0; i < 10; i++) {
    const d = document.createElement('span');
    d.className = 'hled' + (i >= 8 ? ' off' : '');   // mock: 8/10 hull
    wrap.appendChild(d);
  }
})();

/* ---------------- deck tabs ---------------- */
document.querySelectorAll('#deck-tabs .tab').forEach(t => {
  t.onclick = () => {
    document.querySelectorAll('#deck-tabs .tab').forEach(x => x.classList.remove('active'));
    document.querySelectorAll('.deck').forEach(d => d.classList.remove('active'));
    t.classList.add('active');
    document.getElementById('deck-' + t.dataset.d).classList.add('active');
  };
});

/* press-latch helper */
function holdFx(el, onDown, onUp) {
  el.addEventListener('pointerdown', (e) => { e.preventDefault(); el.classList.add('pressed'); onDown && onDown(); });
  ['pointerup', 'pointercancel', 'pointerleave'].forEach(ev =>
    el.addEventListener(ev, () => { el.classList.remove('pressed'); onUp && onUp(); }));
}

/* ============ SHIELD — breaker panel ============ */
const seesaw = document.getElementById('seesaw');
const shlL = document.getElementById('shl-l'), shlR = document.getElementById('shl-r');
function tilt(side) {
  seesaw.classList.toggle('tilt-l', side === 'L');
  seesaw.classList.toggle('tilt-r', side === 'R');
  shlL.classList.toggle('pressed', side === 'L');
  shlR.classList.toggle('pressed', side === 'R');
  if (fightOn) key(side);                       // fight: latch the deflector side
}
shlL.onpointerdown = (e) => { e.preventDefault(); tilt('L'); };
shlR.onpointerdown = (e) => { e.preventDefault(); tilt('R'); };

/* rotary frequency knob: drag rotates the stem through 4 detents */
const knob = document.getElementById('freq-knob');
const stem = document.getElementById('knob-stem');
const DETENT_ANG = [-60, -20, 20, 60];       // stem angles for 1..4
let freq = 1;
function setFreq(f) {
  freq = f;
  stem.style.transform = `rotate(${DETENT_ANG[f - 1] + 180}deg)`;
  document.querySelectorAll('#knob-detents span').forEach(s =>
    s.classList.toggle('on', +s.dataset.f === f));
  if (fightOn) key(f);                          // fight: set the frequency dial
}
knob.addEventListener('pointerdown', (e) => { e.preventDefault(); knob.setPointerCapture(e.pointerId); });
knob.addEventListener('pointermove', (e) => {
  if (e.buttons === 0) return;
  const r = knob.getBoundingClientRect();
  const a = Math.atan2(e.clientY - (r.top + r.height / 2), e.clientX - (r.left + r.width / 2)) * 180 / Math.PI;
  // map pointer angle (stem points down at rest) to nearest detent
  const rel = a - 90;                        // 0 = straight down
  let best = 1, bd = 1e9;
  DETENT_ANG.forEach((d, i) => { const diff = Math.abs(rel - d); if (diff < bd) { bd = diff; best = i + 1; } });
  if (best !== freq) setFreq(best);
});
document.querySelectorAll('#knob-detents span').forEach(s =>
  s.addEventListener('pointerdown', (e) => { e.preventDefault(); setFreq(+s.dataset.f); }));
setFreq(3);

/* overcharge: hold fills the bar; release drains the capacitor (cooldown) */
const overBtn = document.getElementById('shl-over');
const overFill = document.getElementById('over-fill');
const capFill = document.getElementById('cap-fill');
let overT = null, overPct = 0;
holdFx(overBtn,
  () => { overT = setInterval(() => {
      overPct = Math.min(100, overPct + 7);
      overFill.style.width = overPct + '%';
      if (overPct >= 100 && fightOn && !overBtn.dataset.armed) {
        overBtn.dataset.armed = '1';
        key('O');                               // fight: overcharge armed
      }
    }, 60); },
  () => {
    clearInterval(overT);
    if (overPct >= 100) {                    // fired: capacitor drains, refills over 6s
      capFill.style.transition = 'none'; capFill.style.width = '0%';
      requestAnimationFrame(() => { capFill.style.transition = 'width 6s linear'; capFill.style.width = '100%'; });
    }
    overPct = 0; overFill.style.width = '0%';
    delete overBtn.dataset.armed;
  });

/* ============ GUNNER — artillery station ============ */
const crank = document.getElementById('crank');
let crankPct = 0, lastAng = null, spinVel = 0;
crank.addEventListener('pointerdown', (e) => { e.preventDefault(); lastAng = null; crank.setPointerCapture(e.pointerId); });
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
let crankSent = 0;                              // fight: 'K' per 10% actually cranked
function bumpCrank(amt) {
  crankPct = Math.min(100, crankPct + amt);
  crank.style.background = `conic-gradient(var(--accent) ${crankPct * 3.6}deg, var(--surface2) 0deg)`;
  crank.querySelector('span').innerHTML = `crank<br>${Math.round(crankPct)}%`;
  if (fightOn) {
    const steps = Math.floor(crankPct / 10);
    while (crankSent < steps) { crankSent++; key('K'); }
  }
}
(function momentum() {                       // the flywheel keeps spinning briefly
  if (spinVel > 0.3) { spinVel *= 0.92; bumpCrank(spinVel * 0.4); }
  requestAnimationFrame(momentum);
})();

const shellPing = document.getElementById('shell-ping');
const shellHeavy = document.getElementById('shell-heavy');
document.getElementById('mock-load').onclick = () => {
  if (!shellPing.classList.contains('loaded')) shellPing.classList.add('loaded');
  else if (!shellHeavy.classList.contains('loaded')) shellHeavy.classList.add('loaded');
  else { shellPing.classList.remove('loaded'); shellHeavy.classList.remove('loaded'); }
};

const gunL = document.getElementById('gun-l'), gunR = document.getElementById('gun-r');
const lamps = ['lampL', 'lampM', 'lampR'].map(id => document.getElementById(id));
let aim = 1;
function setAim(a) {
  aim = a;
  lamps.forEach((l, i) => l.classList.toggle('on', i === a));
  gunL.classList.toggle('pressed', a === 0);
  gunR.classList.toggle('pressed', a === 2);
}
gunL.onpointerdown = (e) => { e.preventDefault(); setAim(Math.max(0, aim - 1)); };
gunR.onpointerdown = (e) => { e.preventDefault(); setAim(Math.min(2, aim + 1)); };

const fireCover = document.getElementById('fire-cover');
const fireBtn = document.getElementById('fire');
fireCover.addEventListener('pointerdown', (e) => { e.preventDefault(); fireCover.classList.add('open'); });
holdFx(fireBtn, () => {
  const deck = document.getElementById('deck-gun');
  deck.classList.remove('kick'); void deck.offsetWidth; deck.classList.add('kick');
  if (fightOn) key('F');                     // fight: the device checks the charge
  if (crankPct >= 100) {                     // spend a shell + the charge
    (shellHeavy.classList.contains('loaded') ? shellHeavy : shellPing).classList.remove('loaded');
    crankPct = 0; crankSent = 0; bumpCrank(0);
  }
  setTimeout(() => fireCover.classList.remove('open'), 900);   // cover snaps back
});

/* ============ HACKER — terminal ============ */
const GLYPHS = '◇◆▲▽□■○●';
const lines = [0, 1, 2].map(i => document.getElementById('gl' + i));
let offs = [0, 3, 6];
setInterval(() => {
  lines.forEach((el, i) => {
    offs[i] = (offs[i] + 1) % GLYPHS.length;
    let s = '';
    for (let k = 0; k < 6; k++) s += GLYPHS[(offs[i] + k * 2 + i) % GLYPHS.length];
    el.textContent = s;
  });
}, 450);
setInterval(() => {                          // live line wanders; sometimes it flickers (feint demo)
  const live = Math.floor(Math.random() * 3);
  const feint = Math.random() < 0.3;
  lines.forEach((el, i) => {
    el.classList.toggle('live', i === live);
    el.classList.toggle('flicker', i === live && feint);
  });
}, 3500);

/* waveform strip: square wave at the "boss frequency" (mock: matches knob) */
const wf = document.getElementById('waveform');
(function drawWave() {
  const ctx = wf.getContext('2d');
  const t = performance.now() / 1000;
  ctx.fillStyle = '#0d0d0d'; ctx.fillRect(0, 0, wf.width, wf.height);
  ctx.strokeStyle = '#f5a623'; ctx.lineWidth = 2; ctx.beginPath();
  const period = 60 / (freq * 1.6 + 1);      // knob freq shapes the wave — a teaching aid
  for (let x = 0; x < wf.width; x++) {
    const phase = ((x + t * 40) % period) / period;
    const y = phase < 0.5 ? 6 : wf.height - 6;
    x === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
  }
  ctx.stroke();
  requestAnimationFrame(drawWave);
})();

const scanBtn = document.getElementById('scan'), scanArc = document.getElementById('scan-arc');
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

/* codebook: glyph→freq mapping, reshuffled per page load ("per fight") */
(function codebook() {
  const cb = document.getElementById('codebook');
  const picks = [...'◇◆▲▽'].sort(() => Math.random() - 0.5);
  picks.forEach((g, i) => {
    const s = document.createElement('span');
    s.innerHTML = `${g}&thinsp;=&thinsp;<b>${i + 1}</b>`;
    cb.appendChild(s);
  });
})();

/* ============ MEDIC — workbench ============ */
let forgeNext = 1;
const sendBtn = document.getElementById('send-shell');
document.querySelectorAll('.node').forEach(n => {
  n.onpointerdown = (e) => {
    e.preventDefault();
    if (+n.dataset.n === forgeNext) { n.classList.add('lit'); forgeNext++; }
    else { document.querySelectorAll('.node').forEach(x => x.classList.remove('lit')); forgeNext = 1; }
    // 1→2 alone is a valid PING forge; 1→4 is a heavy
    sendBtn.disabled = !(forgeNext === 3 || forgeNext === 5);
    sendBtn.textContent = forgeNext === 3 ? 'send ping' : forgeNext === 5 ? 'send heavy' : 'send to gunner';
  };
});
sendBtn.onclick = () => {
  const heavy = forgeNext === 5;
  document.querySelectorAll('.node').forEach(x => x.classList.remove('lit'));
  forgeNext = 1; sendBtn.disabled = true; sendBtn.textContent = 'send to gunner';
  const cap = document.getElementById('capsule');
  cap.classList.remove('whoosh'); void cap.offsetWidth; cap.classList.add('whoosh');
  setTimeout(() => {                          // …thunks into the gunner's breach
    (heavy ? shellHeavy : shellPing).classList.add('loaded');
    cap.classList.remove('whoosh');
  }, 600);
};

/* triage board: mock alerts wander */
setInterval(() => {
  const icons = ['tri-shl', 'tri-gun', 'tri-hck', 'tri-med'].map(id => document.getElementById(id));
  icons.forEach(i => i.classList.remove('alert'));
  if (Math.random() < 0.6) icons[Math.floor(Math.random() * 4)].classList.add('alert');
}, 4000);

/* wipe pad */
const pad = document.getElementById('wipe-pad');
function spawnSlime() {
  if (pad.querySelectorAll('.slime').length >= 4) return;
  const s = document.createElement('div');
  s.className = 'slime';
  s.style.left = (5 + Math.random() * 70) + '%';
  s.style.top = (8 + Math.random() * 55) + '%';
  s.dataset.hp = 6;
  pad.appendChild(s);
}
setInterval(spawnSlime, 2600); spawnSlime(); spawnSlime();
pad.addEventListener('pointermove', (e) => {
  if (e.buttons === 0) return;
  pad.querySelectorAll('.slime').forEach(s => {
    const r = s.getBoundingClientRect();
    if (e.clientX >= r.left - 6 && e.clientX <= r.right + 6 &&
        e.clientY >= r.top - 6 && e.clientY <= r.bottom + 6) {
      if (--s.dataset.hp <= 0) s.remove();
      else s.style.opacity = s.dataset.hp / 6;
    }
  });
});

/* repair: hold fills the collar */
const repBtn = document.getElementById('repair'), collar = document.getElementById('rep-collar');
let repT = null, rep = 0;
holdFx(repBtn,
  () => { repT = setInterval(() => {
      rep = Math.min(100, rep + 4);
      collar.style.background = `conic-gradient(var(--accent) ${rep * 3.6}deg, var(--surface) 0deg)`;
    }, 200); },
  () => { clearInterval(repT); if (rep >= 100) rep = 0;
          if (!rep) collar.style.background = ''; });

/* ============ PILOT — cockpit ============ */
/* the alert diamond points at a random quadrant every few seconds */
const quads = ['q-shl', 'q-gun', 'q-hck', 'q-med'].map(id => document.getElementById(id));
const diamond = document.getElementById('alert-diamond');
const DIA_POS = [                            // toward each quadrant
  'translate(-140%,-140%) rotate(45deg)', 'translate(40%,-140%) rotate(45deg)',
  'translate(-140%,40%) rotate(45deg)',   'translate(40%,40%) rotate(45deg)'];
setInterval(() => {
  const q = Math.floor(Math.random() * 4);
  quads.forEach((el, i) => el.classList.toggle('alert', i === q));
  diamond.style.transform = DIA_POS[q];
}, 3000);
/* mini glyphs tick */
setInterval(() => {
  const g = document.getElementById('mini-glyph');
  g.textContent = [...'◇◆▲▽□■'].sort(() => Math.random() - 0.5).slice(0, 3).join('');
}, 1200);
/* pilot mini buttons: press feedback only */
document.querySelectorAll('#deck-pil .segmented-btn').forEach(b =>
  b.addEventListener('pointerdown', (e) => {
    e.preventDefault();
    b.classList.add('active'); setTimeout(() => b.classList.remove('active'), 200);
  }));

/* ---------------- boot ---------------- */
buildBossStrip();
buildAnimGrid();
connectWS();
