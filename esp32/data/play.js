/* Frekvens — play page. Phone becomes a controller: it starts a game on the
   device and streams input events over the WebSocket. Games are device-side;
   this page just fetches the registry (/api/games) and renders a controller.

   Zone mode: when the device has zone mode on, each panel can run its own
   game independently (assigned by the admin in the Zones tab) — the play
   page then lists those RUNNING zone games instead of the whole-canvas
   registry (starting a whole-canvas game while zone mode is on would be a
   dead end: main.cpp doesn't render dispState content while zoneModeOn, so
   nothing would ever appear). Joining a zone game never sends a "start"
   command — it's already running — the phone just becomes a controller,
   tagging every input with which zone (panel) it targets. */

let ws = null;
let curGame = null;           // wire name of the running game
let curZoneSlot = -1;         // -1 = legacy whole-canvas game, 0..3 = a zone's game
let previewW = 16, previewH = 16;

// --- WebSocket ---
function connectWS() {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  ws = new WebSocket(`${proto}://${location.host}/ws`);
  ws.binaryType = 'arraybuffer';
  ws.onopen  = () => setConn(true);
  ws.onclose = () => { setConn(false); setTimeout(connectWS, 2000); };
  ws.onmessage = (ev) => {
    if (ev.data instanceof ArrayBuffer) handleFrame(new Uint8Array(ev.data));
  };
}
function send(obj) {
  if (ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(obj));
}
function setConn(on) {
  const el = document.getElementById('play-conn');
  el.textContent = on ? 'online' : 'offline';
  el.className = `chip ${on ? 'online' : 'offline'}`;
}

// --- Preview (mirrors the panel so you can play looking at your phone) ---
function handleFrame(buf) {
  if (buf[0] !== 0x01) return;
  const w = buf[1], h = buf[2], px = buf.subarray(3);
  const canvas = document.getElementById('play-preview');
  if (w !== previewW || h !== previewH || canvas.width !== w * 12) {
    previewW = w; previewH = h;
    canvas.width = w * 12; canvas.height = h * 12;
    // keep it large but within the viewport
    const disp = Math.min(320, Math.round(window.innerWidth * 0.8));
    canvas.style.width = `${disp}px`;
    canvas.style.height = `${Math.round(disp * h / w)}px`;
  }
  const ctx = canvas.getContext('2d');
  ctx.fillStyle = '#000';
  ctx.fillRect(0, 0, canvas.width, canvas.height);
  for (let y = 0; y < h; y++)
    for (let x = 0; x < w; x++) {
      const v = px[y * w + x];
      ctx.fillStyle = v === 0 ? '#111' : `rgb(${v},${Math.round(v * 0.7)},0)`;
      ctx.fillRect(x * 12 + 1, y * 12 + 1, 10, 10);
    }
}

// --- Input helpers ---
// One event: player index, key (U/D/L/R/A), pressed, and (zone mode only)
// which zone/panel it targets. zoneSlot defaults to whatever game is
// currently joined (set by startGame) — every existing bindHold/bindTap/
// enableSwipe call site below is unaware of zones and just calls input()
// with 3 args, which is exactly what should happen; JS re-evaluates a
// default parameter at each call, so this always reads the current value.
// The device expires held keys after 1.2 s, so held controls (Pong paddles)
// re-send while down.
function input(player, key, pressed, zoneSlot = curZoneSlot) {
  const msg = { cmd: 'input', p: player, k: key, s: pressed ? 1 : 0 };
  if (zoneSlot >= 0) msg.z = zoneSlot;
  send(msg);
}

// Wire a button as a momentary "hold": press on pointerdown, release on up,
// and re-send the press on a timer so a lost release can't stick, and the
// device's hold-expiry never fires mid-hold. repeatMs tunes the auto-repeat
// rate — 250 is fine for Pong paddles; Tetris shift/soft-drop want it faster.
function bindHold(el, player, key, repeatMs = 250) {
  let timer = null;
  const down = (e) => {
    e.preventDefault();
    if (timer) return;
    input(player, key, true);
    timer = setInterval(() => input(player, key, true), repeatMs);
    el.classList.add('pressed');
  };
  const up = (e) => {
    if (e) e.preventDefault();
    if (!timer) return;
    clearInterval(timer); timer = null;
    input(player, key, false);
    el.classList.remove('pressed');
  };
  el.addEventListener('pointerdown', down);
  el.addEventListener('pointerup', up);
  el.addEventListener('pointercancel', up);
  el.addEventListener('pointerleave', up);
}

// Wire a button as a single tap (Snake steering — direction latches device-side).
function bindTap(el, player, key) {
  const tap = (e) => { e.preventDefault(); input(player, key, true); flash(el); };
  el.addEventListener('pointerdown', tap);
}
function flash(el) {
  el.classList.add('pressed');
  setTimeout(() => el.classList.remove('pressed'), 90);
}

// --- Game picker ---
function gameTile(title, players, onClick) {
  const b = document.createElement('button');
  b.className = 'game-tile';
  b.innerHTML = `<span class="game-title">${title}</span>` +
                `<span class="game-players">${players > 1 ? players + ' players' : '1 player'}</span>`;
  b.onclick = onClick;
  return b;
}

async function loadGames() {
  const list = document.getElementById('game-list');
  list.innerHTML = '<p class="hint">Loading…</p>';
  try {
    const [gamesResp, zonesResp] = await Promise.all([
      fetch('/api/games').then(r => r.json()).catch(() => ({ games: [] })),
      fetch('/api/zones').then(r => r.json()).catch(() => ({ on: false, zones: [] })),
    ]);
    list.innerHTML = '';

    // Raid 16 needs its own multi-deck page (raid.html) — the generic single
    // d-pad+A fallback controller here would be a dead end, not a downgrade.
    if (gamesResp.games) gamesResp.games = gamesResp.games.filter(g => g.name !== 'raid');

    if (zonesResp.on) {
      // Zone mode: only RUNNING zone games are joinable — the whole-canvas
      // registry would be a dead end while zone mode is on (nothing renders
      // dispState-driven content in that mode).
      const gameNames = new Map((gamesResp.games || []).map(g => [g.name, g]));
      (zonesResp.zones || []).forEach((z, slot) => {
        if (z.mode !== 'game') return;
        const info = gameNames.get(z.sub) || { name: z.sub, title: z.sub, players: 1 };
        list.appendChild(gameTile(`${info.title} — Panel ${slot}`, info.players,
                                  () => startGame(info, slot)));
      });
      if (!list.children.length)
        list.innerHTML = '<p class="hint">Zone mode is on, but no panel is currently ' +
                          'running a game — ask whoever\'s at the panel to set one to ' +
                          '"Game" in the Zones tab.</p>';
      return;
    }

    (gamesResp.games || []).forEach(g => list.appendChild(
      gameTile(g.title, g.players, () => startGame(g))));
    if (!list.children.length)
      list.innerHTML = '<p class="hint">No games available.</p>';
  } catch (_) {
    list.innerHTML = '<p class="hint">Could not reach the panel.</p>';
  }
}

// zoneSlot: -1 (default) = the legacy whole-canvas game — this call actually
// STARTS it. >=0 = a zone game that's already running (assigned by the admin
// in the Zones tab) — joining just wires up a controller, no start command.
function startGame(g, zoneSlot = -1) {
  curGame = g.name;
  curZoneSlot = zoneSlot;
  if (zoneSlot < 0) send({ cmd: 'game', name: g.name });
  document.getElementById('play-picker').style.display = 'none';
  document.getElementById('play-controller').style.display = 'block';
  document.getElementById('ctrl-title').textContent =
    zoneSlot >= 0 ? `${g.title} — Panel ${zoneSlot}` : g.title;
  const body = document.getElementById('ctrl-body');
  body.innerHTML = '';
  (CONTROLLERS[g.name] || CONTROLLERS._default)(body, g);
}

document.getElementById('ctrl-back').addEventListener('click', () => {
  curGame = null;
  curZoneSlot = -1;
  document.getElementById('play-controller').style.display = 'none';
  document.getElementById('play-picker').style.display = 'block';
  loadGames();   // refresh — zone assignments may have changed while playing
});

// --- Controllers: one renderer per game. Add a game to the device registry,
// then (optionally) add a matching entry here; unknown games get the d-pad. ---
const CONTROLLERS = {
  // Snake — a 4-way d-pad plus swipe-to-steer on the preview.
  snake(body) {
    body.innerHTML = `
      <div class="dpad">
        <button class="dbtn up"    data-k="U">&#9650;</button>
        <button class="dbtn left"  data-k="L">&#9664;</button>
        <button class="dbtn right" data-k="R">&#9654;</button>
        <button class="dbtn down"  data-k="D">&#9660;</button>
      </div>
      <p class="hint" style="text-align:center">Tap the pad or swipe the screen above.</p>`;
    body.querySelectorAll('.dbtn').forEach(b => bindTap(b, 0, b.dataset.k));
    enableSwipe(0);
  },

  // Pong — pick a side (that paddle is yours; the other is AI until a second
  // phone claims it), then hold Up/Down.
  pong(body) {
    body.innerHTML = `
      <div class="side-pick" id="side-pick">
        <p class="hint" style="text-align:center">Choose your paddle:</p>
        <div class="side-row">
          <button class="btn-primary side-btn" data-p="0">LEFT</button>
          <button class="btn-primary side-btn" data-p="1">RIGHT</button>
        </div>
      </div>
      <div class="updown" id="updown" style="display:none">
        <button class="bigbtn" data-k="U">&#9650;<span>UP</span></button>
        <button class="bigbtn" data-k="D">&#9660;<span>DOWN</span></button>
      </div>`;
    body.querySelectorAll('.side-btn').forEach(sb => {
      sb.onclick = () => {
        const p = +sb.dataset.p;
        document.getElementById('side-pick').style.display = 'none';
        const ud = document.getElementById('updown');
        ud.style.display = 'flex';
        ud.querySelectorAll('.bigbtn').forEach(b => bindHold(b, p, b.dataset.k));
      };
    });
  },

  // Tetris — d-pad where Left/Right shift (hold to repeat), Down soft-drops
  // (hold), Up rotates (tap, so a hold doesn't spin the piece), plus a big
  // hard-drop button.
  tetris(body) {
    body.innerHTML = `
      <div class="dpad">
        <button class="dbtn up"    data-k="U" title="Rotate">&#8635;</button>
        <button class="dbtn left"  data-k="L">&#9664;</button>
        <button class="dbtn right" data-k="R">&#9654;</button>
        <button class="dbtn down"  data-k="D" title="Soft drop">&#9660;</button>
      </div>
      <button class="bigbtn wide" data-k="A">DROP</button>
      <p class="hint" style="text-align:center">&#9664;&#9654; move · &#8635; rotate · &#9660; soft drop · DROP hard drop</p>`;
    bindHold(body.querySelector('.dbtn.left'),  0, 'L', 120);   // DAS-style shift
    bindHold(body.querySelector('.dbtn.right'), 0, 'R', 120);
    bindHold(body.querySelector('.dbtn.down'),  0, 'D', 80);    // held soft drop
    bindTap(body.querySelector('.dbtn.up'),     0, 'U');        // rotate = discrete
    bindTap(body.querySelector('.bigbtn'),      0, 'A');        // hard drop = discrete
  },

  // Pip Run — one button. Tap to hop; after a crash, tap to restart. Sends 'A'
  // (the device jumps on any press, but 'A' keeps it explicit).
  dino(body) {
    body.innerHTML = `
      <button class="bigbtn wide" data-k="A" style="padding:56px 34px">JUMP</button>
      <p class="hint" style="text-align:center">tap to hop over the cactus · tap to restart after a crash</p>`;
    bindTap(body.querySelector('.bigbtn'), 0, 'A');
  },

  // Flappy Pip — one button. Tap to flap; tap to restart after a crash.
  flappy(body) {
    body.innerHTML = `
      <button class="bigbtn wide" data-k="A" style="padding:56px 34px">FLAP</button>
      <p class="hint" style="text-align:center">tap to flap through the gaps · tap to restart after a crash</p>`;
    bindTap(body.querySelector('.bigbtn'), 0, 'A');
  },

  // Fallback for any future game with no bespoke controller yet.
  _default(body) {
    body.innerHTML = `
      <div class="dpad">
        <button class="dbtn up"    data-k="U">&#9650;</button>
        <button class="dbtn left"  data-k="L">&#9664;</button>
        <button class="dbtn right" data-k="R">&#9654;</button>
        <button class="dbtn down"  data-k="D">&#9660;</button>
      </div>
      <button class="bigbtn wide" data-k="A">A</button>`;
    body.querySelectorAll('.dbtn').forEach(b => bindTap(b, 0, b.dataset.k));
    bindTap(body.querySelector('.bigbtn'), 0, 'A');
  },
};

// Swipe-to-steer on the preview screen (Snake). Latches the dominant axis.
function enableSwipe(player) {
  const el = document.getElementById('play-preview');
  let sx = 0, sy = 0, active = false;
  el.addEventListener('pointerdown', (e) => { active = true; sx = e.clientX; sy = e.clientY; });
  el.addEventListener('pointerup', (e) => {
    if (!active) return;
    active = false;
    const dx = e.clientX - sx, dy = e.clientY - sy;
    if (Math.abs(dx) < 12 && Math.abs(dy) < 12) return;
    if (Math.abs(dx) > Math.abs(dy)) input(player, dx > 0 ? 'R' : 'L', true);
    else                            input(player, dy > 0 ? 'D' : 'U', true);
  });
}

// --- boot ---
loadGames();
connectWS();
