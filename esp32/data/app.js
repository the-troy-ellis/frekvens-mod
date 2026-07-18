/* Frekvens Controller — front-end */

const PANEL_W = 16;

let ws           = null;
let numPanels    = 1;
let canvasW      = 16;    // logical canvas = bounding box of the arrangement
let canvasH      = 16;
let layout       = [{ x: 0, y: 0, rot: 0 }];   // per chain position
let panelMask    = null;  // Uint8Array canvasW*canvasH: 1 = a panel shows this px
let brightness   = 255;
let powerOn      = true;
let currentMode  = 'text';

// Pixels in gaps (no panel there — e.g. a speaker module between panels) are
// rendered dimmed in the preview: visible enough to show content passing
// "behind" the gap, distinct enough to match what the hardware shows.
function rebuildPanelMask() {
  panelMask = new Uint8Array(canvasW * canvasH);
  layout.slice(0, numPanels).forEach(p => {
    for (let y = 0; y < 16; y++)
      for (let x = 0; x < 16; x++) {
        const cx = p.x + x, cy = p.y + y;
        if (cx >= 0 && cx < canvasW && cy >= 0 && cy < canvasH)
          panelMask[cy * canvasW + cx] = 1;
      }
  });
}

// --- WebSocket ---

function connectWS() {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  ws = new WebSocket(`${proto}://${location.host}/ws`);

  ws.onopen = () => {
    setStatus(true);
    fetchConfig();
    fetchImageList();
    fetchAnimList();
  };

  ws.onclose = () => {
    setStatus(false);
    setTimeout(connectWS, 3000);
  };

  ws.onmessage = (ev) => {
    if (ev.data instanceof Blob) {
      ev.data.arrayBuffer().then(handleBinaryFrame);
    } else {
      try {
        const msg = JSON.parse(ev.data);
        if (msg.type === 'zones') handleZonesMsg(msg);
        else                      handleStateMsg(msg);
      } catch (_) {}
    }
  };
}

function sendWS(obj) {
  if (ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(obj));
}

// --- Binary frame: type(1) + w(1) + h(1) + 8-bit pixels (1 byte each) ---
function handleBinaryFrame(buf) {
  const view = new Uint8Array(buf);
  if (view[0] !== 0x01) return;
  const w = view[1];
  const h = view[2];
  drawPreview(view.subarray(3), w, h);
}

const EFFECT_NAMES = [
  '__snake__', '__pong__', '__bounce__', '__sand__', '__life__', '__heart__', '__critter__',
  '__bars__', '__scope__', '__disco__',
  '__rain__', '__fire__', '__ripple__', '__blobs__', '__sparkle__', '__fireworks__',
  '__warp__', '__radar__', '__plasma__', '__static__', '__sweep__', '__identify__',
];

// True until the first state message after page load. The first one syncs the
// whole UI (tab, inputs, brightness) so a reload lands on what the device is
// actually doing; later echoes must NOT touch the controls — syncing them from
// state echoes fought in-progress edits (and the brightness-drag jump-back bug).
let uiSyncedFromDevice = false;

function handleStateMsg(msg) {
  if (msg.type !== 'state') return;
  powerOn = msg.power;
  document.getElementById('power').checked = powerOn;
  // Keep the play trackers current so brightness re-sends target the right file.
  if (msg.mode === 'animation') currentAnimName  = msg.name || '';
  if (msg.mode === 'image')     currentImageName = msg.name || '';
  if (!uiSyncedFromDevice) {
    uiSyncedFromDevice = true;
    syncUiFromState(msg);
  }
}

function syncUiFromState(msg) {
  if (typeof msg.brightness === 'number') {
    brightness = msg.brightness;
    document.getElementById('brightness').value = brightness;
    document.getElementById('brightness-val').textContent = brightness;
  }
  if (msg.mode === 'text') {
    document.getElementById('text-input').value = msg.text ?? '';
    if (msg.scroll_ms) document.getElementById('scroll-ms').value = msg.scroll_ms;
    if (msg.scroll_mode) {
      textScrollMode = msg.scroll_mode;
      document.querySelectorAll('#text-scroll-mode .segmented-btn')
        .forEach(b => b.classList.toggle('active', b.dataset.mode === textScrollMode));
    }
    document.getElementById('text-static').checked = !!msg.static;
    document.getElementById('text-once').checked   = !!msg.once;
    document.getElementById('text-gap').value      = msg.gap_px ?? 0;
    setActiveTab('text');
  } else if (msg.mode === 'clock') {
    clockH24 = msg.h24 !== false;
    document.querySelectorAll('#clock-format .segmented-btn')
      .forEach(b => b.classList.toggle('active', (b.dataset.h24 === '1') === clockH24));
    setActiveTab('clock');
  } else if (msg.mode === 'weather') {
    weatherCond = msg.condition || 'auto';
    document.querySelectorAll('#weather-grid .segmented-btn')
      .forEach(b => b.classList.toggle('active', b.dataset.cond === weatherCond));
    setActiveTab('weather');
  } else if (msg.mode === 'animation' && EFFECT_NAMES.includes(msg.name)) {
    currentEffectName = msg.name;
    if (msg.frame_ms) document.getElementById('effect-ms').value = msg.frame_ms;
    document.querySelectorAll('.effect-grid .segmented-btn')
      .forEach(b => b.classList.toggle('active', b.dataset.effect === msg.name));
    setActiveTab('effects');
  } else if (msg.mode === 'animation') {
    if (msg.frame_ms) document.getElementById('anim-frame-ms').value = msg.frame_ms;
    document.getElementById('anim-loop').checked = msg.loop !== false;
    setActiveTab('animation');
  } else if (msg.mode === 'image') {
    setActiveTab('image');
  }
  // mode 'off': leave the default tab — selecting the Off tab is what *sends* off.
}

// --- LED preview canvas ---
function drawPreview(pixels, w, h) {
  const canvas = document.getElementById('preview');
  const ctx    = canvas.getContext('2d');
  const scaleX = canvas.width  / w;
  const scaleY = canvas.height / h;

  ctx.clearRect(0, 0, canvas.width, canvas.height);

  const maskOk = panelMask && panelMask.length === w * h;
  // pixels: 1 byte per pixel, 0-255 (8-bit depth)
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const lum = pixels[y * w + x];
      const mapped = !maskOk || panelMask[y * w + x];
      let color;
      if (mapped) color = lum === 0 ? '#111' : `rgb(${lum},${Math.round(lum * 0.7)},0)`;
      else        color = lum === 0 ? '#0a0a0a'   // gap, dark
                        : `rgb(${Math.round(lum * 0.2)},${Math.round(lum * 0.16)},${Math.round(lum * 0.1)})`;
      ctx.fillStyle = color;
      ctx.fillRect(Math.floor(x * scaleX) + 1, Math.floor(y * scaleY) + 1,
                   Math.floor(scaleX) - 1, Math.floor(scaleY) - 1);
    }
  }
}

// Demo: draw a static placeholder grid when no live data yet
function drawPlaceholder() {
  const canvas = document.getElementById('preview');
  const ctx    = canvas.getContext('2d');
  const cw = canvas.width, ch = canvas.height;
  const sx = cw / canvasW, sy = ch / canvasH;
  ctx.fillStyle = '#0d0d0d';
  ctx.fillRect(0, 0, cw, ch);
  const maskOk = panelMask && panelMask.length === canvasW * canvasH;
  for (let y = 0; y < canvasH; y++) {
    for (let x = 0; x < canvasW; x++) {
      const mapped = !maskOk || panelMask[y * canvasW + x];
      ctx.fillStyle = mapped ? '#1a1a1a' : '#0a0a0a';
      ctx.fillRect(Math.floor(x*sx)+1, Math.floor(y*sy)+1, Math.floor(sx)-1, Math.floor(sy)-1);
    }
  }
}

// --- Status ---
function setStatus(online) {
  const el = document.getElementById('conn-status');
  el.textContent = online ? 'Online' : 'Offline';
  el.className   = `chip ${online ? 'online' : 'offline'}`;
}

// --- Config fetch ---
async function fetchConfig() {
  try {
    const r = await fetch('/api/config');
    const d = await r.json();
    numPanels = d.num_panels ?? 1;
    brightness = d.brightness ?? 255;
    document.getElementById('brightness').value = brightness;
    document.getElementById('brightness-val').textContent = brightness;
    document.getElementById('cfg-name').value         = d.device_name ?? '';
    document.getElementById('cfg-panels').value       = d.num_panels  ?? 1;
    document.getElementById('cfg-mqtt-enabled').checked = d.mqtt_enabled ?? false;
    document.getElementById('cfg-mqtt-broker').value  = d.mqtt_broker ?? '';
    document.getElementById('cfg-mqtt-port').value    = d.mqtt_port   ?? 1883;
    document.getElementById('cfg-mqtt-user').value    = d.mqtt_user   ?? '';
    document.getElementById('cfg-tz').value           = d.tz          ?? '';
    document.getElementById('cfg-gamma').checked      = d.gamma       ?? false;
    document.getElementById('cfg-critter').checked    = d.critter     ?? true;
    document.getElementById('cfg-zone-mode').checked  = d.zone_mode   ?? false;
    canvasW = d.canvas_w ?? PANEL_W;
    canvasH = d.canvas_h ?? numPanels * PANEL_W;
    layout  = (d.layout && d.layout.length)
            ? d.layout.map(p => ({ x: p.x ?? 0, y: p.y ?? 0, rot: p.rot ?? 0 }))
            : Array.from({ length: numPanels }, (_, i) => ({ x: 0, y: i * 16, rot: 0 }));
    rebuildPanelMask();
    renderLayoutEditor();
    await fetchZones();
    await refreshStatus();

    resizePreviewCanvas();
    resizeDrawCanvas();
    drawPlaceholder();
  } catch (_) {}
}

function fmtBytes(n) {
  if (n == null) return '';
  if (n >= 1024 * 1024) return (n / 1024 / 1024).toFixed(1) + ' MB';
  if (n >= 1024)        return (n / 1024).toFixed(1) + ' KB';
  return n + ' B';
}

function fmtUptime(s) {
  if (s == null) return '';
  const d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60);
  return d ? `${d}d ${h}h` : h ? `${h}h ${m}m` : `${m}m`;
}

// Storage bar + device info in Settings. Called on load and after any
// upload/delete (via the list refreshes) so the bar tracks flash usage.
async function refreshStatus() {
  try {
    const r = await fetch('/api/status');
    const s = await r.json();
    document.getElementById('ip-label').textContent = s.ip ?? '';
    if (s.fs_total) {
      const pct = Math.round(s.fs_used / s.fs_total * 100);
      document.getElementById('storage-fill').style.width = pct + '%';
      document.getElementById('device-info').innerHTML =
        `Storage: ${fmtBytes(s.fs_used)} / ${fmtBytes(s.fs_total)} (${pct}%)<br>` +
        `Uptime: ${fmtUptime(s.uptime_s)} &nbsp;·&nbsp; Heap: ${fmtBytes(s.heap)}<br>` +
        `Firmware: ${s.version ?? '?'}`;
    }
  } catch (_) {}
}

function resizePreviewCanvas() {
  const canvas = document.getElementById('preview');
  // Canvas takes the arrangement's shape; keep the displayed size sane.
  canvas.width  = canvasW * 10;
  canvas.height = canvasH * 10;
  const dispW = Math.min(330, Math.round(176 * canvasW / 16));
  canvas.style.width  = `${dispW}px`;
  canvas.style.height = `${Math.round(dispW * canvasH / canvasW)}px`;
  drawPlaceholder();
}

// --- Image list ---
async function fetchImageList() {
  try {
    const r = await fetch('/api/images');
    const d = await r.json();
    renderFileList('image-list', d.images ?? [], 'image');
    // Cached for the Zones tab's Image sub-picker (needs just the names, and
    // fetching its own copy every row-render would be wasteful).
    window.__lastImageNames = (d.images ?? []).map(e => e.name ?? e);
    refreshStatus();   // keep the storage bar current after uploads/deletes
  } catch (_) {}
}

async function fetchAnimList() {
  try {
    const r = await fetch('/api/anims');
    const d = await r.json();
    renderFileList('anim-list', d.anims ?? [], 'animation');
    refreshStatus();
  } catch (_) {}
}

function renderFileList(listId, entries, type) {
  const ul = document.getElementById(listId);
  ul.innerHTML = '';
  if (!entries.length) {
    ul.innerHTML = '<li style="color:var(--text-dim);border:none;background:none">No files saved</li>';
    return;
  }
  entries.forEach(entry => {
    // Entries are {name, size} objects; tolerate plain strings for safety.
    const name = entry.name ?? entry;
    const size = entry.size;
    const li = document.createElement('li');
    li.innerHTML = `
      <span>${name}<span class="file-size">${size != null ? fmtBytes(size) : ''}</span></span>
      <span>
        <button class="play-btn" title="Play">▶</button>
        <button class="edit-btn" title="Open in the Draw editor">✎</button>
        <button class="del-btn" title="Delete">✕</button>
      </span>`;
    li.querySelector('.play-btn').onclick = () => playFile(type, name);
    li.querySelector('.edit-btn').onclick = () =>
      type === 'image' ? loadSavedImage(name) : loadSavedAnim(name);
    li.querySelector('.del-btn').onclick  = () => deleteFile(type, name, li);
    ul.appendChild(li);
  });
}

function playFile(type, name) {
  if (type === 'image') {
    currentImageName = name;
    sendWS({ cmd: 'image', name, brightness });
  } else {
    currentAnimName = name;
    sendWS({ cmd: 'animation', name,
             frame_ms: +document.getElementById('anim-frame-ms').value,
             loop: document.getElementById('anim-loop').checked, brightness });
  }
  setActiveTab(type);
}

async function deleteFile(type, name, li) {
  const endpoint = type === 'image' ? '/api/images/delete' : '/api/anims/delete';
  await fetch(endpoint, { method: 'POST', body: JSON.stringify({ name }),
                          headers: { 'Content-Type': 'application/json' } });
  li.remove();
}

// --- Image upload + client-side resize/quantize ---
let lastLoadedImg = null;   // kept so toggling Invert can re-convert live

document.getElementById('img-file').addEventListener('change', function() {
  const file = this.files[0];
  if (!file) return;
  const reader = new FileReader();
  reader.onload = (e) => {
    const img = new Image();
    img.onload = () => { lastLoadedImg = img; quantizeImage(img); };
    img.src = e.target.result;
  };
  reader.readAsDataURL(file);
});

// Re-convert the current image when the Invert toggle changes.
document.getElementById('img-invert').addEventListener('change', () => {
  if (lastLoadedImg) quantizeImage(lastLoadedImg);
});

let processedImageData = null;

function quantizeImage(img) {
  const off = document.createElement('canvas');
  off.width = canvasW; off.height = canvasH;
  const ctx = off.getContext('2d');
  // Composite over black so PNG transparency becomes "off" pixels. The canvas is
  // transparent by default; without this, transparent regions read as
  // browser-dependent RGB and the grayscale comes out wrong (JPGs are opaque so
  // they were unaffected).
  ctx.fillStyle = '#000';
  ctx.fillRect(0, 0, canvasW, canvasH);
  ctx.drawImage(img, 0, 0, canvasW, canvasH);
  const imageData = ctx.getImageData(0, 0, canvasW, canvasH).data;

  // Convert to 8-bit grayscale, 1 byte per pixel (0-255).
  // Full 8-bit depth to take advantage of ATtiny1614 256-level brightness.
  const invert = document.getElementById('img-invert').checked;
  const pixels = new Uint8Array(canvasW * canvasH);
  for (let i = 0; i < canvasW * canvasH; i++) {
    const r = imageData[i * 4], g = imageData[i * 4 + 1], b = imageData[i * 4 + 2];
    let gray = Math.round(0.299 * r + 0.587 * g + 0.114 * b);
    if (invert) gray = 255 - gray;
    pixels[i] = gray;
  }
  processedImageData = pixels;

  // Show in preview canvas
  const prev = document.getElementById('img-preview');
  prev.width  = canvasW * 10;
  prev.height = canvasH * 10;
  const dispW = Math.min(320, Math.round(160 * canvasW / 16));
  prev.style.width  = `${dispW}px`;
  prev.style.height = `${Math.round(dispW * canvasH / canvasW)}px`;
  renderPixelsToCanvas(prev, pixels, canvasW, canvasH);
  document.getElementById('img-preview-wrap').style.display = 'block';
  document.getElementById('upload-img').disabled = false;
}

// LittleFS caps filenames at 31 chars — strip the extension, keep safe chars,
// and truncate so the stored name (base + ".raw"/".anim") fits.
function safeName(fn, max = 24) {
  const base = (fn || 'file').replace(/\.[^.]*$/, '')
                             .replace(/[^A-Za-z0-9_-]/g, '_')
                             .slice(0, max);
  return base || 'file';
}

document.getElementById('upload-img').addEventListener('click', async () => {
  if (!processedImageData) return;
  const storedName = safeName(document.getElementById('img-file').files[0]?.name) + '.raw';
  const formData = new FormData();
  formData.append('file', new Blob([processedImageData], { type: 'application/octet-stream' }), storedName);
  await fetch('/api/images/upload', { method: 'POST', body: formData });
  currentImageName = storedName;
  sendWS({ cmd: 'image', name: currentImageName, brightness });
  fetchImageList();
});

// --- Animation upload (raw GIF bytes, server-side extraction placeholder) ---
document.getElementById('anim-file').addEventListener('change', function() {
  if (this.files[0]) document.getElementById('upload-anim').disabled = false;
});

// Grey-levels slider: 2..16 posterises; the top notch (17) means "Off" (full range).
document.getElementById('anim-levels').addEventListener('input', function() {
  const raw = +this.value;
  document.getElementById('anim-levels-val').textContent = raw > 16 ? 'Off' : raw;
});

document.getElementById('upload-anim').addEventListener('click', async () => {
  const file = document.getElementById('anim-file').files[0];
  if (!file) return;
  const formData = new FormData();
  // Keep ".gif" so the server decodes it, but sanitize/truncate the base name.
  formData.append('file', file, safeName(file.name) + '.gif');
  // Invert + grey-level reduction are applied server-side during decode (baked into
  // the .anim). Build the query string from both options.
  const params = new URLSearchParams();
  if (document.getElementById('anim-invert').checked) params.set('invert', '1');
  const rawLevels = +document.getElementById('anim-levels').value;
  if (rawLevels <= 16) params.set('levels', rawLevels);   // 17 = Off (no quantization)
  const qs = params.toString() ? '?' + params.toString() : '';
  const resp = await fetch('/api/anims/upload' + qs, { method: 'POST', body: formData });
  const data = await resp.json().catch(() => ({}));
  // Server returns the stored .anim filename — use it, not the original .gif name.
  const storedName = data.name ?? file.name;
  currentAnimName  = storedName;
  const frameMs = +document.getElementById('anim-frame-ms').value;
  const loop    = document.getElementById('anim-loop').checked;
  sendWS({ cmd: 'animation', name: storedName, frame_ms: frameMs, loop, brightness });
  fetchAnimList();
});

// --- Draw mode --------------------------------------------------------------
// A small pixel editor: tools (brush/eraser/line/rect/box/ring/fill/pick),
// mirror + dither painting, stroke undo/redo, a frame timeline with per-frame
// delays and onion skin, text stamping with the device's own font, image
// import / PNG export, and a Live mode that paints straight onto the panel.

// The device's 5x7 font (96 glyphs x 5 column bytes, bit 0 = top row),
// mirrored here so "Stamp" renders exactly what the firmware would.
const FONT5X7 = Uint8Array.from(atob(
  'AAAAAAAAAF8AAAAHAAcAFH8UfxQkKn8qEiMTCGRiNklVIlAABQMAAAAcIkEAAEEiHAAIKhwq' +
  'CAgIPggIAFAwAAAICAgICABgYAAAIBAIBAI+UUlFPgBCf0AAQmFRSUYhQUVLMRgUEn8QJ0VF' +
  'RTk8SklJMAFxCQUDNklJSTYGSUkpHgA2NgAAAFY2AAAACBQiQRQUFBQUQSIUCAACAVEJBjJJ' +
  'eUE+fhEREX5/SUlJNj5BQUEif0FBIhx/SUlJQX8JCQEBPkFBUTJ/CAgIfwBBf0EAIEBBPwF/' +
  'CBQiQX9AQEBAfwIEAn9/BAgQfz5BQUE+fwkJCQY+QVEhXn8JGSlGRklJSTEBAX8BAT9AQEA/' +
  'HyBAIB8/QDhAP2MUCBRjAwR4BANhUUlFQwAAf0FBAgQIECBBQX8AAAQCAQIEQEBAQEAAAQIE' +
  'ACBUVFR4f0hERDg4REREIDhEREh/OFRUVBgIfgkBAggUVFQ8fwgEBHgARH1AACBARD0AAH8Q' +
  'KEQAQX9AAHwEGAR4fAgEBHg4REREOHwUFBQICBQUGHx8CAQECEhUVFQgBD9EQCA8QEAgfBwg' +
  'QCAcPEAwQDxEKBAoRAxQUFA8RGRUTEQACDZBAAAAfwAAAEE2CAAIBAgQCAAAAAAA'
), c => c.charCodeAt(0));

// Initialized immediately (16x16 default) — the module's bottom-of-file UI
// setup runs before boot calls resizeDrawCanvas, which re-inits on size change.
let drawFrames = [{ px: new Uint8Array(16 * 16), delay: 100 }];
let curFrame   = 0;
let drawPixels = drawFrames[0].px;   // alias of drawFrames[curFrame].px
let drawBrush  = 255;
let drawTool   = 'brush';
let mirrorH = false, mirrorV = false, ditherOn = false, liveDraw = false;

let undoStack = [], redoStack = [];   // pixel-edit snapshots {frame, px}
const UNDO_MAX = 60;

let strokeActive = false;
let strokeErase  = false;   // right-button (or eraser tool) stroke
let shapeStart = null, shapeEnd = null;   // rubber band for line/rect/box/ring

let thumbCanvases = [];     // frame-strip canvas elements, by frame index

function dEl(id) { return document.getElementById(id); }

function resizeDrawCanvas() {
  const neededLen = canvasW * canvasH;
  // Only reset on a real size change — this runs on every reconnect, and
  // rebuilding unconditionally used to wipe in-progress drawings on WiFi blips.
  if (!drawPixels || drawPixels.length !== neededLen) {
    drawFrames = [{ px: new Uint8Array(neededLen), delay: 100 }];
    curFrame   = 0;
    drawPixels = drawFrames[0].px;
    undoStack = []; redoStack = [];
    renderFrameStrip();
  }
  const canvas = dEl('draw-canvas');
  canvas.width  = canvasW * 10;
  canvas.height = canvasH * 10;
  const dispW = Math.min(320, Math.round(160 * canvasW / 16));
  canvas.style.width  = `${dispW}px`;
  canvas.style.height = `${Math.round(dispW * canvasH / canvasW)}px`;
  dEl('draw-rot').disabled = canvasW !== canvasH;   // 90° needs a square canvas
  redrawDrawCanvas();
}

function renderPixelsToCanvas(canvasEl, pixels, w, h) {
  const ctx = canvasEl.getContext('2d');
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const lum = pixels[y * w + x];
      ctx.fillStyle = lum === 0 ? '#111' : `rgb(${lum},${Math.round(lum * 0.7)},0)`;
      ctx.fillRect(x * 10, y * 10, 10, 10);
    }
  }
}

// Main editor canvas: gap-dimmed background, onion-skinned previous frame,
// current pixels, shape rubber band, and panel boundaries.
function redrawDrawCanvas() {
  const canvas = dEl('draw-canvas');
  const ctx = canvas.getContext('2d');
  const onion  = dEl('draw-onion').checked && curFrame > 0 ? drawFrames[curFrame - 1].px : null;
  const maskOk = panelMask && panelMask.length === canvasW * canvasH;
  const overlay = (shapeStart && shapeEnd) ? shapePixels(drawTool, shapeStart, shapeEnd) : null;
  ctx.fillStyle = '#000';
  ctx.fillRect(0, 0, canvas.width, canvas.height);
  for (let y = 0; y < canvasH; y++) {
    for (let x = 0; x < canvasW; x++) {
      const i = y * canvasW + x;
      const mapped = !maskOk || panelMask[i];
      const v = drawPixels[i];
      let style;
      if (overlay && overlay.has(i)) {
        style = 'rgb(120,220,255)';                    // rubber band
      } else if (v > 0) {
        style = mapped ? `rgb(${v},${Math.round(v * 0.7)},0)`
                       : `rgb(${Math.round(v * 0.25)},${Math.round(v * 0.18)},${Math.round(v * 0.1)})`;
      } else if (onion && onion[i] > 0) {
        const g = Math.max(30, Math.round(onion[i] * 0.3));   // ghost: cool blue-grey
        style = `rgb(${Math.round(g * 0.45)},${Math.round(g * 0.6)},${g})`;
      } else {
        style = mapped ? '#111' : '#070707';
      }
      ctx.fillStyle = style;
      ctx.fillRect(x * 10 + 1, y * 10 + 1, 9, 9);
    }
  }
  // Physical panel boundaries (only interesting with 2+ panels)
  if (numPanels > 1) {
    ctx.strokeStyle = 'rgba(245,166,35,0.35)';
    layout.slice(0, numPanels).forEach(p => {
      ctx.strokeRect(p.x * 10 + 0.5, p.y * 10 + 0.5, 159, 159);
    });
  }
}

// --- painting core ---

// Writes one logical pixel (plus its mirror copies), honoring dither mode.
// In Live mode each write is streamed to the physical panel too.
function setPix(x, y, v) {
  if (x < 0 || x >= canvasW || y < 0 || y >= canvasH) return;
  if (ditherOn && ((x + y) & 1)) return;
  const targets = [[x, y]];
  if (mirrorH) targets.push([canvasW - 1 - x, y]);
  if (mirrorV) targets.push([x, canvasH - 1 - y]);
  if (mirrorH && mirrorV) targets.push([canvasW - 1 - x, canvasH - 1 - y]);
  for (const [tx, ty] of targets) {
    const i = ty * canvasW + tx;
    if (drawPixels[i] === v) continue;
    drawPixels[i] = v;
    if (liveDraw) sendWS({ cmd: 'pixel', x: tx, y: ty, v });
  }
}

function shapePixels(tool, a, b) {
  const set = new Set();
  const add = (x, y) => {
    if (x >= 0 && x < canvasW && y >= 0 && y < canvasH) set.add(y * canvasW + x);
  };
  const x0 = Math.min(a.x, b.x), x1 = Math.max(a.x, b.x);
  const y0 = Math.min(a.y, b.y), y1 = Math.max(a.y, b.y);
  if (tool === 'line') {
    let px = a.x, py = a.y;
    const dx = Math.abs(b.x - a.x), dy = -Math.abs(b.y - a.y);
    const sx = a.x < b.x ? 1 : -1, sy = a.y < b.y ? 1 : -1;
    let err = dx + dy;
    for (;;) {
      add(px, py);
      if (px === b.x && py === b.y) break;
      const e2 = 2 * err;
      if (e2 >= dy) { err += dy; px += sx; }
      if (e2 <= dx) { err += dx; py += sy; }
    }
  } else if (tool === 'rect' || tool === 'rectfill') {
    for (let y = y0; y <= y1; y++)
      for (let x = x0; x <= x1; x++)
        if (tool === 'rectfill' || x === x0 || x === x1 || y === y0 || y === y1) add(x, y);
  } else if (tool === 'circle') {
    const cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    const rx = Math.max(0.5, (x1 - x0) / 2), ry = Math.max(0.5, (y1 - y0) / 2);
    for (let t = 0; t < 6.2832; t += 0.02)
      add(Math.round(cx + rx * Math.cos(t)), Math.round(cy + ry * Math.sin(t)));
  }
  return set;
}

function floodFill(x, y, v) {
  const target = drawPixels[y * canvasW + x];
  if (target === v) return;
  const stack = [[x, y]];
  while (stack.length) {
    const [px, py] = stack.pop();
    if (px < 0 || px >= canvasW || py < 0 || py >= canvasH) continue;
    const i = py * canvasW + px;
    if (drawPixels[i] !== target) continue;
    drawPixels[i] = v;
    stack.push([px + 1, py], [px - 1, py], [px, py + 1], [px, py - 1]);
  }
}

// --- undo / redo (pixel edits only; frame add/delete/move clear the history) ---

function snapshotUndo() {
  undoStack.push({ frame: curFrame, px: drawPixels.slice() });
  if (undoStack.length > UNDO_MAX) undoStack.shift();
  redoStack = [];
}

function doUndo() {
  const s = undoStack.pop();
  if (!s || s.frame >= drawFrames.length) return;
  selectFrame(s.frame);
  redoStack.push({ frame: s.frame, px: drawPixels.slice() });
  drawPixels.set(s.px);
  afterBulkEdit();
}

function doRedo() {
  const s = redoStack.pop();
  if (!s || s.frame >= drawFrames.length) return;
  selectFrame(s.frame);
  undoStack.push({ frame: s.frame, px: drawPixels.slice() });
  drawPixels.set(s.px);
  afterBulkEdit();
}

// After any multi-pixel change: repaint, refresh the thumbnail, and (in Live
// mode) push the whole frame to the panel — streaming hundreds of individual
// pixel messages would flood the device's command queue.
function afterBulkEdit() {
  redrawDrawCanvas();
  renderThumb(curFrame);
  pushLiveFrameSoon();
}

// --- pointer handling ---

function cellFromEvent(e) {
  const canvas = dEl('draw-canvas');
  const rect = canvas.getBoundingClientRect();
  const x = Math.floor((e.clientX - rect.left) / rect.width  * canvasW);
  const y = Math.floor((e.clientY - rect.top)  / rect.height * canvasH);
  if (x < 0 || x >= canvasW || y < 0 || y >= canvasH) return null;
  return { x, y };
}

function pickAt(c) {
  setBrush(drawPixels[c.y * canvasW + c.x]);
  if (drawTool === 'pick') setTool('brush');   // one-shot, back to painting
}

const drawCanvasEl = dEl('draw-canvas');
drawCanvasEl.addEventListener('contextmenu', e => e.preventDefault());

drawCanvasEl.addEventListener('pointerdown', (e) => {
  const c = cellFromEvent(e);
  if (!c) return;
  e.preventDefault();
  if (e.altKey || drawTool === 'pick') { pickAt(c); return; }
  if (drawTool === 'fill') {
    snapshotUndo();
    floodFill(c.x, c.y, e.button === 2 ? 0 : drawBrush);
    afterBulkEdit();
    return;
  }
  snapshotUndo();
  strokeErase  = (e.button === 2) || drawTool === 'eraser';
  strokeActive = true;
  drawCanvasEl.setPointerCapture(e.pointerId);
  if (drawTool === 'brush' || drawTool === 'eraser') {
    setPix(c.x, c.y, strokeErase ? 0 : drawBrush);
    redrawDrawCanvas();
  } else {
    shapeStart = c; shapeEnd = c;
    redrawDrawCanvas();
  }
});

drawCanvasEl.addEventListener('pointermove', (e) => {
  const c = cellFromEvent(e);
  if (c) {
    dEl('draw-readout').textContent =
      `${c.x}, ${c.y} · ${drawPixels[c.y * canvasW + c.x]}`;
  }
  if (!strokeActive || !c) return;
  if (shapeStart) {
    shapeEnd = c;
    redrawDrawCanvas();
  } else {
    setPix(c.x, c.y, strokeErase ? 0 : drawBrush);
    redrawDrawCanvas();
  }
});

function endStroke() {
  if (!strokeActive) return;
  strokeActive = false;
  if (shapeStart && shapeEnd) {
    const v = strokeErase ? 0 : drawBrush;
    for (const i of shapePixels(drawTool, shapeStart, shapeEnd))
      setPix(i % canvasW, Math.floor(i / canvasW), v);
    shapeStart = shapeEnd = null;
  }
  afterBulkEdit();
}
drawCanvasEl.addEventListener('pointerup',     endStroke);
drawCanvasEl.addEventListener('pointercancel', () => { strokeActive = false; shapeStart = shapeEnd = null; redrawDrawCanvas(); });
drawCanvasEl.addEventListener('pointerleave',  () => { dEl('draw-readout').innerHTML = '&nbsp;'; });

// --- tools / toggles UI ---

function setTool(t) {
  drawTool = t;
  document.querySelectorAll('#draw-tools .tool-btn')
    .forEach(b => b.classList.toggle('active', b.dataset.tool === t));
}
document.querySelectorAll('#draw-tools .tool-btn').forEach(btn =>
  btn.addEventListener('click', () => setTool(btn.dataset.tool)));

dEl('draw-mirror-h').addEventListener('click', function () {
  mirrorH = !mirrorH; this.classList.toggle('active', mirrorH);
});
dEl('draw-mirror-v').addEventListener('click', function () {
  mirrorV = !mirrorV; this.classList.toggle('active', mirrorV);
});
dEl('draw-dither').addEventListener('click', function () {
  ditherOn = !ditherOn; this.classList.toggle('active', ditherOn);
});
dEl('draw-live').addEventListener('click', function () {
  liveDraw = !liveDraw;
  this.classList.toggle('active', liveDraw);
  if (liveDraw) pushLiveFrame();   // sync the panel to the canvas right away
});

// --- brush swatches + exact value ---

function setBrush(v) {
  drawBrush = Math.max(0, Math.min(255, v));
  dEl('draw-brush-val').value = drawBrush;
  document.querySelectorAll('.swatch').forEach(el =>
    el.classList.toggle('selected', +el.title === drawBrush));
}

function buildBrushSwatches() {
  const row = dEl('draw-swatches');
  row.innerHTML = '';
  for (let step = 0; step <= 15; step++) {
    const v = Math.round(step * 255 / 15);
    // Plain div, not <button> — Safari renders native control chrome under
    // tiny empty buttons even with appearance:none.
    const sw = document.createElement('div');
    sw.setAttribute('role', 'button');
    sw.tabIndex = 0;
    sw.className = 'swatch' + (v === drawBrush ? ' selected' : '');
    sw.style.background = v === 0 ? '#111' : `rgb(${v},${Math.round(v * 0.7)},0)`;
    sw.title = String(v);
    const select = () => setBrush(v);
    sw.addEventListener('click', select);
    sw.addEventListener('keydown', (e) => {
      if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); select(); }
    });
    row.appendChild(sw);
  }
}
dEl('draw-brush-val').addEventListener('change', function () { setBrush(+this.value || 0); });

// --- clear / flood / transforms ---

dEl('draw-undo').addEventListener('click', doUndo);
dEl('draw-redo').addEventListener('click', doRedo);
dEl('draw-clear').addEventListener('click', () => {
  snapshotUndo(); drawPixels.fill(0); afterBulkEdit();
});
dEl('draw-fill-white').addEventListener('click', () => {
  snapshotUndo(); drawPixels.fill(drawBrush); afterBulkEdit();
});

function xform(fn) {
  snapshotUndo();
  const src = drawPixels.slice();
  fn(src);
  afterBulkEdit();
}
function nudge(dx, dy) {
  xform(src => {
    for (let y = 0; y < canvasH; y++)
      for (let x = 0; x < canvasW; x++)
        drawPixels[((y + dy + canvasH) % canvasH) * canvasW + ((x + dx + canvasW) % canvasW)]
          = src[y * canvasW + x];
  });
}
dEl('draw-left').addEventListener('click',  () => nudge(-1, 0));
dEl('draw-right').addEventListener('click', () => nudge(1, 0));
dEl('draw-up').addEventListener('click',    () => nudge(0, -1));
dEl('draw-down').addEventListener('click',  () => nudge(0, 1));
dEl('draw-flip-h').addEventListener('click', () => xform(src => {
  for (let y = 0; y < canvasH; y++)
    for (let x = 0; x < canvasW; x++)
      drawPixels[y * canvasW + (canvasW - 1 - x)] = src[y * canvasW + x];
}));
dEl('draw-flip-v').addEventListener('click', () => xform(src => {
  for (let y = 0; y < canvasH; y++)
    for (let x = 0; x < canvasW; x++)
      drawPixels[(canvasH - 1 - y) * canvasW + x] = src[y * canvasW + x];
}));
dEl('draw-rot').addEventListener('click', () => {
  if (canvasW !== canvasH) return;
  xform(src => {
    for (let y = 0; y < canvasH; y++)
      for (let x = 0; x < canvasW; x++)
        drawPixels[x * canvasW + (canvasW - 1 - y)] = src[y * canvasW + x];   // 90° CW
  });
});
dEl('draw-invert').addEventListener('click', () => xform(src => {
  for (let i = 0; i < src.length; i++) drawPixels[i] = 255 - src[i];
}));

// --- text stamp (device font, centered — nudge into place afterwards) ---

dEl('draw-stamp').addEventListener('click', () => {
  const text = dEl('draw-stamp-text').value;
  if (!text) return;
  snapshotUndo();
  const w  = text.length * 6 - 1;
  const x0 = Math.round((canvasW - w) / 2);
  const y0 = Math.round((canvasH - 7) / 2);
  for (let i = 0; i < text.length; i++) {
    let code = text.charCodeAt(i);
    if (code < 32 || code > 127) code = 63;   // '?'
    for (let col = 0; col < 5; col++) {
      const bits = FONT5X7[(code - 32) * 5 + col];
      for (let row = 0; row < 7; row++)
        if (bits & (1 << row)) {
          const px = x0 + i * 6 + col, py = y0 + row;
          if (px >= 0 && px < canvasW && py >= 0 && py < canvasH)
            drawPixels[py * canvasW + px] = drawBrush;
        }
    }
  }
  afterBulkEdit();
});

// --- import / export ---

dEl('draw-import').addEventListener('change', function () {
  const file = this.files[0];
  if (!file) return;
  const reader = new FileReader();
  reader.onload = (ev) => {
    const img = new Image();
    img.onload = () => {
      snapshotUndo();
      const off = document.createElement('canvas');
      off.width = canvasW; off.height = canvasH;
      const ctx = off.getContext('2d');
      ctx.fillStyle = '#000';
      ctx.fillRect(0, 0, canvasW, canvasH);
      ctx.drawImage(img, 0, 0, canvasW, canvasH);
      const d = ctx.getImageData(0, 0, canvasW, canvasH).data;
      for (let i = 0; i < canvasW * canvasH; i++)
        drawPixels[i] = Math.round(0.299 * d[i * 4] + 0.587 * d[i * 4 + 1] + 0.114 * d[i * 4 + 2]);
      afterBulkEdit();
    };
    img.src = ev.target.result;
  };
  reader.readAsDataURL(file);
  this.value = '';   // allow re-importing the same file
});

function downloadBlob(blob, filename) {
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = filename;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 5000);
}

dEl('draw-export-png').addEventListener('click', () => {
  const scale = 12;
  const off = document.createElement('canvas');
  off.width = canvasW * scale; off.height = canvasH * scale;
  const ctx = off.getContext('2d');
  for (let y = 0; y < canvasH; y++)
    for (let x = 0; x < canvasW; x++) {
      const v = drawPixels[y * canvasW + x];
      ctx.fillStyle = v === 0 ? '#111' : `rgb(${v},${Math.round(v * 0.7)},0)`;
      ctx.fillRect(x * scale, y * scale, scale, scale);
    }
  const name = safeName(dEl('draw-name').value || 'drawing');
  off.toBlob(b => downloadBlob(b, name + '.png'));
});

dEl('draw-export-file').addEventListener('click', () => {
  const name = safeName(dEl('draw-name').value || 'drawing');
  const seq = framesForSend();
  if (seq.length === 1) {
    downloadBlob(new Blob([drawPixels], { type: 'application/octet-stream' }), name + '.raw');
  } else {
    downloadBlob(new Blob([buildAnimBinary(seq, canvasW, canvasH)],
                          { type: 'application/octet-stream' }), name + '.anim');
  }
});

// Load a saved file back into the editor (the web server serves the whole
// filesystem, so /images/x.raw and /anims/x.anim are directly fetchable).
async function loadSavedImage(name) {
  const r = await fetch('/images/' + encodeURIComponent(name));
  if (!r.ok) { alert('Could not fetch ' + name); return; }
  const buf = new Uint8Array(await r.arrayBuffer());
  if (buf.length !== canvasW * canvasH) {
    alert('That image was made for a different panel arrangement (' + buf.length +
          ' px vs ' + canvasW * canvasH + ').');
    return;
  }
  snapshotUndo();
  drawPixels.set(buf);
  afterBulkEdit();
  dEl('draw-name').value = name.replace(/\.raw$/, '');
  setActiveTab('draw');
}

async function loadSavedAnim(name) {
  const r = await fetch('/anims/' + encodeURIComponent(name));
  if (!r.ok) { alert('Could not fetch ' + name); return; }
  const buf = new Uint8Array(await r.arrayBuffer());
  if (buf.length < 8 || String.fromCharCode(buf[0], buf[1], buf[2], buf[3]) !== 'ANIM') {
    alert('Not a valid .anim file.'); return;
  }
  const w = buf[4], h = buf[5], n = buf[6] | (buf[7] << 8);
  if (w !== canvasW || h !== canvasH) {
    alert(`That animation is ${w}×${h} — the canvas is ${canvasW}×${canvasH}.`);
    return;
  }
  const fb = w * h;
  const delayOff = 8 + n * fb;
  drawFrames = [];
  for (let i = 0; i < n; i++) {
    const delay = buf[delayOff + i * 2] | (buf[delayOff + i * 2 + 1] << 8);
    drawFrames.push({ px: buf.slice(8 + i * fb, 8 + (i + 1) * fb), delay: delay || 100 });
  }
  curFrame = 0;
  drawPixels = drawFrames[0].px;
  undoStack = []; redoStack = [];
  renderFrameStrip();
  redrawDrawCanvas();
  dEl('draw-name').value = name.replace(/\.anim$/, '');
  setActiveTab('draw');
}

// --- frame timeline ---

function renderThumb(i) {
  const c = thumbCanvases[i];
  if (!c) return;
  const ctx = c.getContext('2d');
  const img = ctx.createImageData(canvasW, canvasH);
  const px = drawFrames[i].px;
  for (let j = 0; j < px.length; j++) {
    const v = px[j];
    img.data[j * 4]     = v ? v : 17;
    img.data[j * 4 + 1] = v ? Math.round(v * 0.7) : 17;
    img.data[j * 4 + 2] = v ? 0 : 17;
    img.data[j * 4 + 3] = 255;
  }
  ctx.putImageData(img, 0, 0);
}

function renderFrameStrip() {
  const strip = dEl('frame-strip');
  strip.innerHTML = '';
  thumbCanvases = [];
  const th = 36;
  const tw = Math.max(18, Math.min(120, Math.round(th * canvasW / canvasH)));
  drawFrames.forEach((f, i) => {
    const wrap = document.createElement('div');
    wrap.className = 'frame-thumb' + (i === curFrame ? ' selected' : '');
    wrap.style.width = `${tw}px`;
    wrap.style.height = `${th}px`;
    const c = document.createElement('canvas');
    c.width = canvasW; c.height = canvasH;
    c.style.width = '100%';
    c.style.height = '100%';
    c.style.imageRendering = 'pixelated';
    const num = document.createElement('span');
    num.className = 'fnum';
    num.textContent = i;
    wrap.appendChild(c);
    wrap.appendChild(num);
    wrap.addEventListener('click', () => selectFrame(i));
    strip.appendChild(wrap);
    thumbCanvases[i] = c;
    renderThumb(i);
  });
  dEl('frame-del').disabled  = drawFrames.length <= 1;
  dEl('frame-prev').disabled = curFrame === 0;
  dEl('frame-next').disabled = curFrame >= drawFrames.length - 1;
  dEl('draw-anim-delay').value = drawFrames[curFrame].delay;
  updateDrawLocalPreview();
}

function selectFrame(i) {
  curFrame = Math.max(0, Math.min(i, drawFrames.length - 1));
  drawPixels = drawFrames[curFrame].px;
  dEl('draw-anim-delay').value = drawFrames[curFrame].delay;
  document.querySelectorAll('.frame-thumb')
    .forEach((el, j) => el.classList.toggle('selected', j === curFrame));
  dEl('frame-prev').disabled = curFrame === 0;
  dEl('frame-next').disabled = curFrame >= drawFrames.length - 1;
  redrawDrawCanvas();
  pushLiveFrameSoon();
}

dEl('frame-add').addEventListener('click', () => {
  drawFrames.splice(curFrame + 1, 0,
                    { px: drawPixels.slice(), delay: drawFrames[curFrame].delay });
  undoStack = []; redoStack = [];
  selectFrame(curFrame + 1);
  renderFrameStrip();
});
dEl('frame-del').addEventListener('click', () => {
  if (drawFrames.length <= 1) return;
  drawFrames.splice(curFrame, 1);
  undoStack = []; redoStack = [];
  selectFrame(Math.min(curFrame, drawFrames.length - 1));
  renderFrameStrip();
});
dEl('frame-prev').addEventListener('click', () => {
  if (curFrame === 0) return;
  [drawFrames[curFrame - 1], drawFrames[curFrame]] = [drawFrames[curFrame], drawFrames[curFrame - 1]];
  undoStack = []; redoStack = [];
  selectFrame(curFrame - 1);
  renderFrameStrip();
});
dEl('frame-next').addEventListener('click', () => {
  if (curFrame >= drawFrames.length - 1) return;
  [drawFrames[curFrame + 1], drawFrames[curFrame]] = [drawFrames[curFrame], drawFrames[curFrame + 1]];
  undoStack = []; redoStack = [];
  selectFrame(curFrame + 1);
  renderFrameStrip();
});
dEl('draw-anim-delay').addEventListener('input', function () {
  drawFrames[curFrame].delay = Math.max(20, Math.min(10000, +this.value || 100));
});
dEl('draw-onion').addEventListener('change', redrawDrawCanvas);
dEl('draw-pingpong').addEventListener('change', updateDrawLocalPreview);

// --- local animation preview (client-side only, never touches the panel) ---

let previewPlaying = true;
let previewIdx = 0;
let previewTimer = null;

function updateDrawLocalPreview() {
  clearTimeout(previewTimer);
  previewTimer = null;
  const wrap = dEl('draw-anim-preview-wrap');
  if (drawFrames.length < 2) { wrap.style.display = 'none'; return; }
  wrap.style.display = 'block';
  const canvas = dEl('draw-anim-preview');
  canvas.width  = canvasW * 10;
  canvas.height = canvasH * 10;
  const dispW = Math.min(320, Math.round(160 * canvasW / 16));
  canvas.style.width  = `${dispW}px`;
  canvas.style.height = `${Math.round(dispW * canvasH / canvasW)}px`;
  const tick = () => {
    const seq = framesForSend();
    if (previewIdx >= seq.length) previewIdx = 0;
    renderPixelsToCanvas(canvas, seq[previewIdx].px, canvasW, canvasH);
    if (previewPlaying)
      previewTimer = setTimeout(() => { previewIdx++; tick(); },
                                Math.max(20, seq[previewIdx].delay));
  };
  tick();
}
dEl('prev-play').addEventListener('click', () => {
  previewPlaying = !previewPlaying;
  updateDrawLocalPreview();
});
dEl('prev-step-b').addEventListener('click', () => {
  previewPlaying = false;
  previewIdx = (previewIdx - 1 + framesForSend().length) % framesForSend().length;
  updateDrawLocalPreview();
});
dEl('prev-step-f').addEventListener('click', () => {
  previewPlaying = false;
  previewIdx = (previewIdx + 1) % framesForSend().length;
  updateDrawLocalPreview();
});

// --- building & sending ---

// The frames as they will play: timeline order, plus baked ping-pong.
function framesForSend() {
  let seq = drawFrames.map(f => ({ px: f.px, delay: f.delay }));
  if (dEl('draw-pingpong').checked && seq.length > 2)
    seq = seq.concat(seq.slice(1, -1).reverse());
  return seq;
}

// The exact on-device .anim binary (see gif_decoder.h AnimHeader):
//   [4] "ANIM", [1] w, [1] h, [2] num_frames LE,
//   [n*w*h] pixels, [n*2] per-frame delay_ms LE
function buildAnimBinary(seq, w, h) {
  const frameBytes = w * h;
  const buf  = new Uint8Array(8 + seq.length * frameBytes + seq.length * 2);
  const view = new DataView(buf.buffer);
  buf.set([0x41, 0x4E, 0x49, 0x4D], 0);
  buf[4] = w; buf[5] = h;
  view.setUint16(6, seq.length, true);
  let off = 8;
  for (const f of seq) { buf.set(f.px, off); off += frameBytes; }
  for (const f of seq) { view.setUint16(off, f.delay, true); off += 2; }
  return buf;
}

dEl('draw-send').addEventListener('click', async () => {
  if (drawPreviewing) {
    drawPreviewing = false;
    dEl('draw-preview-anim').textContent = 'Preview on Panel';
  }
  const seq = framesForSend();
  const baseName = safeName(dEl('draw-name').value || 'drawing');
  if (seq.length === 1) {
    const storedName = baseName + '.raw';
    const formData = new FormData();
    formData.append('file', new Blob([seq[0].px], { type: 'application/octet-stream' }), storedName);
    await fetch('/api/images/upload', { method: 'POST', body: formData });
    currentImageName = storedName;
    sendWS({ cmd: 'image', name: currentImageName, brightness });
    fetchImageList();
  } else {
    const storedName = baseName + '.anim';
    const binary = buildAnimBinary(seq, canvasW, canvasH);
    const formData = new FormData();
    formData.append('file', new Blob([binary], { type: 'application/octet-stream' }), storedName);
    const resp = await fetch('/api/anims/upload_raw', { method: 'POST', body: formData });
    const data = await resp.json().catch(() => ({}));
    if (!data.ok) { alert(data.error || 'Upload failed'); return; }
    currentAnimName = storedName;
    sendWS({ cmd: 'animation', name: storedName, frame_ms: 100, loop: true, brightness });
    fetchAnimList();
  }
});

// --- panel preview (uploads to a hidden __preview__ file, plays live) ---
const DRAW_PREVIEW_NAME = '__preview__.anim';
let drawPreviewing = false;

async function startDrawPreview() {
  const binary = buildAnimBinary(framesForSend(), canvasW, canvasH);
  const formData = new FormData();
  formData.append('file', new Blob([binary], { type: 'application/octet-stream' }), DRAW_PREVIEW_NAME);
  const resp = await fetch('/api/anims/upload_raw', { method: 'POST', body: formData });
  const data = await resp.json().catch(() => ({}));
  if (!data.ok) { alert(data.error || 'Preview upload failed'); return; }
  sendWS({ cmd: 'animation', name: DRAW_PREVIEW_NAME, frame_ms: 100, loop: true, brightness });
  drawPreviewing = true;
  dEl('draw-preview-anim').textContent = 'Stop Preview';
}

function stopDrawPreview() {
  sendWS({ cmd: 'off' });
  drawPreviewing = false;
  dEl('draw-preview-anim').textContent = 'Preview on Panel';
}

dEl('draw-preview-anim').addEventListener('click', () => {
  if (drawPreviewing) stopDrawPreview();
  else                startDrawPreview();
});

// --- live mode full-frame push (bulk edits; strokes stream per-pixel) ---
let livePushTimer = null;
function pushLiveFrameSoon() {
  if (!liveDraw) return;
  clearTimeout(livePushTimer);
  livePushTimer = setTimeout(pushLiveFrame, 350);
}
async function pushLiveFrame() {
  if (!liveDraw) return;
  const formData = new FormData();
  formData.append('file', new Blob([drawPixels], { type: 'application/octet-stream' }), '__live__.raw');
  await fetch('/api/images/upload', { method: 'POST', body: formData });
  sendWS({ cmd: 'image', name: '__live__.raw', brightness });
}

// --- keyboard shortcuts (Draw tab only, not while typing) ---
document.addEventListener('keydown', (e) => {
  if (!dEl('pane-draw').classList.contains('active')) return;
  const t = e.target.tagName;
  if (t === 'INPUT' || t === 'TEXTAREA' || t === 'SELECT') return;
  if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'z') {
    e.preventDefault();
    e.shiftKey ? doRedo() : doUndo();
    return;
  }
  const tools = { b: 'brush', e: 'eraser', l: 'line', r: 'rect', f: 'fill' };
  const k = e.key.toLowerCase();
  if (tools[k]) { setTool(tools[k]); return; }
  const nudges = { ArrowLeft: [-1, 0], ArrowRight: [1, 0], ArrowUp: [0, -1], ArrowDown: [0, 1] };
  if (nudges[e.key]) { e.preventDefault(); nudge(...nudges[e.key]); }
});

buildBrushSwatches();
renderFrameStrip();

// --- Controls ---
document.getElementById('brightness').addEventListener('input', function() {
  brightness = +this.value;
  document.getElementById('brightness-val').textContent = brightness;
  sendCurrentMode();
});

document.getElementById('power').addEventListener('change', function() {
  powerOn = this.checked;
  // On power-on, resend the full mode command (with its content fields) so the
  // panel resumes what it was showing, not a blank frame.
  if (powerOn) sendCurrentMode();
  else         sendWS({ cmd: 'off', brightness });
});

// --- Text scroll mode picker ---
// A fixed set of 5 named (glyph orientation, travel direction) combos — just
// tracks a selected value client-side; the actual scroll math lives entirely
// in firmware (tickDisplay/main.cpp).
let textScrollMode = 'horizontal_west';

document.querySelectorAll('#text-scroll-mode .segmented-btn').forEach(btn => {
  btn.addEventListener('click', () => {
    textScrollMode = btn.dataset.mode;
    document.querySelectorAll('#text-scroll-mode .segmented-btn')
      .forEach(b => b.classList.toggle('active', b === btn));
  });
});

function buildTextCmd() {
  return {
    cmd: 'text',
    text: document.getElementById('text-input').value,
    brightness,
    scroll_ms: +document.getElementById('scroll-ms').value,
    scroll_mode: textScrollMode,
    static: document.getElementById('text-static').checked,
    once: document.getElementById('text-once').checked,
    gap_px: +document.getElementById('text-gap').value || 0,
  };
}

document.getElementById('send-text').addEventListener('click', () => {
  sendWS(buildTextCmd());
});

// --- Clock mode ---
let clockH24 = true;

document.querySelectorAll('#clock-format .segmented-btn').forEach(btn => {
  btn.addEventListener('click', () => {
    clockH24 = btn.dataset.h24 === '1';
    document.querySelectorAll('#clock-format .segmented-btn')
      .forEach(b => b.classList.toggle('active', b === btn));
  });
});

function buildClockCmd() {
  return { cmd: 'clock', h24: clockH24, brightness };
}

document.getElementById('send-clock').addEventListener('click', () => {
  sendWS(buildClockCmd());
});

// --- Weather ambience (scene picked by condition; "auto" tracks HA's forecast) ---
let weatherCond = 'auto';

function buildWeatherCmd() {
  return { cmd: 'weather', condition: weatherCond, brightness };
}

document.querySelectorAll('#weather-grid .segmented-btn').forEach(btn => {
  btn.addEventListener('click', () => {
    weatherCond = btn.dataset.cond;
    document.querySelectorAll('#weather-grid .segmented-btn')
      .forEach(b => b.classList.toggle('active', b === btn));
    sendWS(buildWeatherCmd());
  });
});

// --- Effects (built into the firmware; reserved __name__ animations) ---
let currentEffectName = '';

document.querySelectorAll('.effect-grid .segmented-btn').forEach(btn => {
  btn.addEventListener('click', () => {
    currentEffectName = btn.dataset.effect;
    document.querySelectorAll('.effect-grid .segmented-btn')
      .forEach(b => b.classList.toggle('active', b === btn));
    sendWS({ cmd: 'animation', name: currentEffectName,
             frame_ms: +document.getElementById('effect-ms').value || 80,
             loop: true, brightness });
  });
});

// Track what's currently playing so brightness changes can re-send it
let currentAnimName  = '';
let currentImageName = '';

function sendCurrentMode() {
  if (currentMode === 'text') {
    sendWS(buildTextCmd());
  } else if (currentMode === 'clock') {
    sendWS(buildClockCmd());
  } else if (currentMode === 'weather') {
    sendWS(buildWeatherCmd());
  } else if (currentMode === 'effects' && currentEffectName) {
    sendWS({ cmd: 'animation', name: currentEffectName,
             frame_ms: +document.getElementById('effect-ms').value || 80,
             loop: true, brightness });
  } else if (currentMode === 'image' && currentImageName) {
    sendWS({ cmd: 'image', name: currentImageName, brightness });
  } else if (currentMode === 'animation' && currentAnimName) {
    sendWS({ cmd: 'animation', name: currentAnimName,
             frame_ms: +document.getElementById('anim-frame-ms').value,
             loop: document.getElementById('anim-loop').checked, brightness });
  } else if (currentMode === 'draw') {
    if (drawPreviewing) {
      // A panel preview is active — keep it playing, just with the new brightness.
      // Without this, a brightness change here would fall through to resending
      // whatever static image was last sent (or nothing), knocking the preview
      // animation off the panel.
      sendWS({ cmd: 'animation', name: DRAW_PREVIEW_NAME, frame_ms: 100, loop: true, brightness });
    } else if (liveDraw) {
      // Live drawing shows the hidden __live__ image — keep it, new brightness.
      sendWS({ cmd: 'image', name: '__live__.raw', brightness });
    } else if (currentImageName) {
      sendWS({ cmd: 'image', name: currentImageName, brightness });
    }
  }
}

// --- Tabs ---
function setActiveTab(mode) {
  // Note: switching UI tabs never sends device commands on its own (matches every
  // other mode) — a panel preview started from Draw keeps playing until you click
  // Stop Preview or send something else, same as any other mode would.
  currentMode = mode;
  document.querySelectorAll('.tab').forEach(t => t.classList.toggle('active', t.dataset.mode === mode));
  document.querySelectorAll('.mode-pane').forEach(p => p.classList.toggle('active', p.id === `pane-${mode}`));
}

document.querySelectorAll('.tab').forEach(tab => {
  tab.addEventListener('click', () => {
    setActiveTab(tab.dataset.mode);
    if (tab.dataset.mode === 'off') sendWS({ cmd: 'off' });
  });
});

// --- Panel arrangement editor ---
// One row of X / Y / rotation inputs per chain position, plus a to-scale
// sketch of the arrangement. The firmware normalizes on save (clamps, shifts
// the bounding box to 0,0), so the editor stays simple.
function renderLayoutEditor() {
  const wrap = document.getElementById('layout-rows');
  wrap.innerHTML = '';
  const n = Math.min(4, Math.max(1, +document.getElementById('cfg-panels').value || 1));
  while (layout.length < n) layout.push({ x: 0, y: layout.length * 16, rot: 0 });
  layout.length = n;
  layout.forEach((p, i) => {
    const row = document.createElement('div');
    row.className = 'layout-row';
    row.innerHTML = `
      <span class="layout-idx">#${i}</span>
      <label>X <input type="number" data-i="${i}" data-k="x" value="${p.x}" min="0" max="112" step="1"></label>
      <label>Y <input type="number" data-i="${i}" data-k="y" value="${p.y}" min="0" max="112" step="1"></label>
      <select data-i="${i}" data-k="rot" title="Physical mounting rotation (clockwise)">
        <option value="0">0&deg;</option><option value="1">90&deg;</option>
        <option value="2">180&deg;</option><option value="3">270&deg;</option>
      </select>`;
    row.querySelector('select').value = p.rot;
    wrap.appendChild(row);
  });
  wrap.querySelectorAll('input,select').forEach(el => {
    el.addEventListener('input', () => {
      layout[+el.dataset.i][el.dataset.k] = +el.value || 0;
      drawLayoutSketch();
    });
  });
  drawLayoutSketch();
}

function drawLayoutSketch() {
  const c = document.getElementById('layout-preview');
  let maxX = 16, maxY = 16;
  layout.forEach(p => { maxX = Math.max(maxX, p.x + 16); maxY = Math.max(maxY, p.y + 16); });
  const scale = Math.min(220 / maxX, 160 / maxY, 8);
  c.width  = Math.ceil(maxX * scale) + 2;
  c.height = Math.ceil(maxY * scale) + 2;
  const ctx = c.getContext('2d');
  ctx.fillStyle = '#0d0d0d';
  ctx.fillRect(0, 0, c.width, c.height);
  layout.forEach((p, i) => {
    const x = p.x * scale + 1, y = p.y * scale + 1, s = 16 * scale;
    ctx.fillStyle   = 'rgba(245,166,35,0.15)';
    ctx.strokeStyle = '#f5a623';
    ctx.fillRect(x, y, s, s);
    ctx.strokeRect(x + 0.5, y + 0.5, s - 1, s - 1);
    // top-edge tick showing where the panel's "up" points after rotation
    ctx.fillStyle = '#f5a623';
    const t = s / 3;
    if      (p.rot == 0) ctx.fillRect(x + s/2 - t/2, y,         t, 2);
    else if (p.rot == 1) ctx.fillRect(x + s - 2,     y + s/2 - t/2, 2, t);
    else if (p.rot == 2) ctx.fillRect(x + s/2 - t/2, y + s - 2, t, 2);
    else                 ctx.fillRect(x,             y + s/2 - t/2, 2, t);
    ctx.fillStyle = '#e8e8e8';
    ctx.font = `${Math.max(9, s / 3)}px sans-serif`;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    ctx.fillText(String(i), x + s / 2, y + s / 2);
  });
}

document.getElementById('cfg-panels').addEventListener('input', renderLayoutEditor);

document.getElementById('identify-btn').addEventListener('click', () => {
  // Each panel shows its chain index + a blinking tick on its physical top edge.
  sendWS({ cmd: 'animation', name: '__identify__', frame_ms: 250, loop: true, brightness });
});

// --- Zones (per-panel content assignment) ---
// zoneSlots[i] = {mode, sub} for chain position i, mirrors the firmware's
// AppConfig.zoneSlots. zoneOptions caches the curated effect/game name lists
// from /api/zones so the sub-pickers don't need their own fetch.
let zoneModeOn  = false;
let zoneSlots   = [{ mode: 'off', sub: '' }, { mode: 'off', sub: '' },
                   { mode: 'off', sub: '' }, { mode: 'off', sub: '' }];
let zoneOptions = { effects: [], games: [] };
const WEATHER_CONDITIONS = ['auto', 'sunny', 'clear-night', 'partlycloudy', 'cloudy', 'fog',
                            'rainy', 'snowy', 'snowy-rainy', 'lightning', 'windy'];

async function fetchZones() {
  try {
    const r = await fetch('/api/zones');
    const d = await r.json();
    zoneModeOn = d.on ?? false;
    document.getElementById('cfg-zone-mode').checked = zoneModeOn;
    if (d.zones) zoneSlots = d.zones.map(z => ({ mode: z.mode ?? 'off', sub: z.sub ?? '' }));
    zoneOptions.effects = d.effects ?? [];
    zoneOptions.games   = d.games   ?? [];
    document.getElementById('zone-mode-warning').style.display = zoneModeOn ? 'none' : '';
    renderZoneRows();
  } catch (_) {}
}

// A {"type":"zones",...} broadcast — e.g. another tab (or the play page,
// indirectly) changed an assignment, or zone mode was toggled via Settings.
function handleZonesMsg(msg) {
  zoneModeOn = msg.on ?? false;
  document.getElementById('cfg-zone-mode').checked = zoneModeOn;
  document.getElementById('zone-mode-warning').style.display = zoneModeOn ? 'none' : '';
  if (msg.zones) zoneSlots = msg.zones.map(z => ({ mode: z.mode ?? 'off', sub: z.sub ?? '' }));
  renderZoneRows();
}

const ZONE_MODE_LABELS = { off: 'Off', clock: 'Clock', weather: 'Weather',
                           effect: 'Effect', game: 'Game', image: 'Image', text: 'Text' };

// Builds the sub-picker for one zone row, matching its current mode. Kept as
// its own function since the picker TYPE changes whenever the mode <select>
// changes (a dropdown for Weather/Effect/Game/Image, a short text input for
// Text, nothing for Off/Clock).
function buildZoneSubControl(slot, mode, currentSub) {
  if (mode === 'weather') {
    const sel = document.createElement('select');
    sel.className = 'zone-sub';
    WEATHER_CONDITIONS.forEach(c => {
      const o = document.createElement('option');
      o.value = c; o.textContent = c === 'auto' ? 'Live (auto)' : c;
      sel.appendChild(o);
    });
    sel.value = currentSub || 'auto';
    return sel;
  }
  if (mode === 'effect') {
    const sel = document.createElement('select');
    sel.className = 'zone-sub';
    zoneOptions.effects.forEach(e => {
      const o = document.createElement('option');
      o.value = e; o.textContent = e[0].toUpperCase() + e.slice(1);
      sel.appendChild(o);
    });
    if (zoneOptions.effects.includes(currentSub)) sel.value = currentSub;
    return sel;
  }
  if (mode === 'game') {
    const sel = document.createElement('select');
    sel.className = 'zone-sub';
    zoneOptions.games.forEach(g => {
      const o = document.createElement('option');
      o.value = g; o.textContent = g[0].toUpperCase() + g.slice(1);
      sel.appendChild(o);
    });
    if (zoneOptions.games.includes(currentSub)) sel.value = currentSub;
    return sel;
  }
  if (mode === 'image') {
    const sel = document.createElement('select');
    sel.className = 'zone-sub';
    const blank = document.createElement('option');
    blank.value = ''; blank.textContent = '(choose an image)';
    sel.appendChild(blank);
    (window.__lastImageNames || []).forEach(n => {
      const o = document.createElement('option');
      o.value = n; o.textContent = n;
      sel.appendChild(o);
    });
    sel.value = currentSub || '';
    sel.title = 'Must be a 16×16 saved image (one panel exactly)';
    return sel;
  }
  if (mode === 'text') {
    const inp = document.createElement('input');
    inp.type = 'text'; inp.className = 'zone-sub'; inp.maxLength = 2;
    inp.placeholder = '2 chars';
    inp.value = currentSub || '';
    return inp;
  }
  return null;   // off / clock: nothing to configure
}

function sendZoneAssign(slot) {
  const z = zoneSlots[slot];
  sendWS({ cmd: 'zone', slot, mode: z.mode, sub: z.sub });
}

function renderZoneRows() {
  const wrap = document.getElementById('zone-rows');
  wrap.innerHTML = '';
  const n = Math.min(4, Math.max(1, numPanels));
  for (let i = 0; i < n; i++) {
    if (!zoneSlots[i]) zoneSlots[i] = { mode: 'off', sub: '' };
    const row = document.createElement('div');
    row.className = 'zone-row';

    const idx = document.createElement('span');
    idx.className = 'zone-idx'; idx.textContent = `#${i}`;
    row.appendChild(idx);

    const modeSel = document.createElement('select');
    modeSel.className = 'zone-mode-select';
    Object.entries(ZONE_MODE_LABELS).forEach(([val, label]) => {
      const o = document.createElement('option');
      o.value = val; o.textContent = label;
      modeSel.appendChild(o);
    });
    modeSel.value = zoneSlots[i].mode;
    row.appendChild(modeSel);

    const subHolder = document.createElement('span');
    subHolder.className = 'zone-sub-holder';
    const sub = buildZoneSubControl(i, zoneSlots[i].mode, zoneSlots[i].sub);
    if (sub) subHolder.appendChild(sub);
    row.appendChild(subHolder);

    modeSel.addEventListener('change', () => {
      zoneSlots[i].mode = modeSel.value;
      subHolder.innerHTML = '';
      const newSub = buildZoneSubControl(i, zoneSlots[i].mode, '');
      // Sync to whatever the fresh control actually defaults to (e.g. Weather's
      // <select> defaults to "auto", Effect's to its first option) — sending a
      // bare "" here would silently mismatch what's visibly selected.
      zoneSlots[i].sub = newSub ? newSub.value : '';
      if (newSub) {
        subHolder.appendChild(newSub);
        newSub.addEventListener('input', () => { zoneSlots[i].sub = newSub.value; sendZoneAssign(i); });
      }
      sendZoneAssign(i);   // Off/Clock/Weather(auto) apply immediately; Text waits for typing
    });
    if (sub) sub.addEventListener('input', () => { zoneSlots[i].sub = sub.value; sendZoneAssign(i); });

    wrap.appendChild(row);
  }
}

document.getElementById('cfg-zone-mode').addEventListener('change', () => {
  // Immediate, like every other live control — doesn't wait for "Save settings".
  fetch('/api/config', {
    method: 'POST',
    body: JSON.stringify({ zone_mode: document.getElementById('cfg-zone-mode').checked }),
    headers: { 'Content-Type': 'application/json' },
  }).then(fetchZones);
});

// --- Settings ---
document.getElementById('save-cfg').addEventListener('click', async () => {
  const cfg = {
    device_name:   document.getElementById('cfg-name').value,
    num_panels:    +document.getElementById('cfg-panels').value,
    mqtt_enabled:  document.getElementById('cfg-mqtt-enabled').checked,
    mqtt_broker:   document.getElementById('cfg-mqtt-broker').value,
    mqtt_port:     +document.getElementById('cfg-mqtt-port').value,
    mqtt_user:     document.getElementById('cfg-mqtt-user').value,
    mqtt_pass:     document.getElementById('cfg-mqtt-pass').value,
    tz:            document.getElementById('cfg-tz').value,
    gamma:         document.getElementById('cfg-gamma').checked,
    critter:       document.getElementById('cfg-critter').checked,
    zone_mode:     document.getElementById('cfg-zone-mode').checked,
    layout:        layout.map(p => ({ x: p.x, y: p.y, rot: p.rot })),
    brightness,
  };
  await fetch('/api/config', { method: 'POST', body: JSON.stringify(cfg),
                               headers: { 'Content-Type': 'application/json' } });
  // Re-pull the config: the firmware normalizes the layout (clamp/shift) and
  // the canvas shape may have changed — this re-sizes every canvas and mask.
  await fetchConfig();
});

// --- Boot ---
drawPlaceholder();
resizeDrawCanvas();
connectWS();
