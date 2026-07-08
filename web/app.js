// Receipt editor: compose text/images on a 384px-wide canvas, dither it,
// and POST the packed 1-bit bitmap to the print relay.

const RELAY_URL =
  ['localhost', '127.0.0.1'].includes(location.hostname) ? 'http://localhost:8377' // local dev
  : location.hostname.endsWith('evankoza.com') ? 'https://print.evankoza.com'      // GitHub Pages
  : '';                                    // page served by the relay itself (tunnel): same origin

const W = 384;
const preview = document.getElementById('preview');
const pctx = preview.getContext('2d');
const overlay = document.getElementById('overlay');
const compose = document.createElement('canvas'); // grayscale source
compose.width = W;
const cctx = compose.getContext('2d', { willReadFrequently: true });

// ---------- state ----------
let elements = [];   // {type:'text'|'image', x, y, ...}
let selected = null;
let paperH = 400;
let elSeq = 1;

const $ = id => document.getElementById(id);

// ---------- element model ----------
function addTextEl() {
  const el = {
    id: elSeq++, type: 'text', text: 'Hello, paper!', x: 20, y: 20,
    size: 24, font: 'monospace', bold: false, align: 'left',
  };
  elements.push(el); select(el); render();
}

function addImageEl(img, name) {
  const scale = Math.min(1, (W - 20) / img.width);
  const el = {
    id: elSeq++, type: 'image', img, name, x: 10, y: 10,
    w: Math.round(img.width * scale), h: Math.round(img.height * scale),
    aspect: img.width / img.height,
  };
  elements.push(el); select(el); render();
}

function elBounds(el) {
  if (el.type === 'image') return { x: el.x, y: el.y, w: el.w, h: el.h };
  cctx.font = `${el.bold ? 'bold ' : ''}${el.size}px ${el.font}`;
  const lines = el.text.split('\n');
  const lineH = el.size * 1.25;
  let w = 0;
  for (const l of lines) w = Math.max(w, cctx.measureText(l).width);
  return { x: el.x, y: el.y, w: Math.ceil(w), h: Math.ceil(lines.length * lineH) };
}

function select(el) {
  selected = el;
  refreshElementList();
  refreshPropsPanel();
  drawSelection();
}

function deleteSelected() {
  if (!selected) return;
  elements = elements.filter(e => e !== selected);
  selected = null;
  refreshElementList(); refreshPropsPanel(); render();
}

// ---------- compositing + dithering ----------
function renderCompose() {
  compose.height = paperH;
  cctx.fillStyle = '#fff';
  cctx.fillRect(0, 0, W, paperH);
  cctx.fillStyle = '#000';
  for (const el of elements) {
    if (el.type === 'image') {
      cctx.drawImage(el.img, el.x, el.y, el.w, el.h);
    } else {
      cctx.font = `${el.bold ? 'bold ' : ''}${el.size}px ${el.font}`;
      cctx.textBaseline = 'top';
      cctx.textAlign = el.align;
      const lineH = el.size * 1.25;
      const b = elBounds(el);
      const ax = el.align === 'center' ? el.x + b.w / 2 : el.align === 'right' ? el.x + b.w : el.x;
      el.text.split('\n').forEach((line, i) => cctx.fillText(line, ax, el.y + i * lineH));
    }
  }
}

const BAYER4 = [0,8,2,10, 12,4,14,6, 3,11,1,9, 15,7,13,5].map(v => (v + .5) / 16);
const BAYER8 = (() => {
  const b2 = [0, 2, 3, 1]; // recursive 2x2 base pattern
  const out = new Array(64);
  for (let y = 0; y < 8; y++) for (let x = 0; x < 8; x++) {
    let v = 0;
    for (let bit = 0, xs = x, ys = y; bit < 3; bit++, xs >>= 1, ys >>= 1)
      v = v * 4 + b2[((ys & 1) << 1) | (xs & 1)];
    out[y * 8 + x] = (v + .5) / 64;
  }
  return out;
})();

// Returns Uint8Array of 0/1 (1 = black), length W*paperH.
function ditherToBits() {
  renderCompose();
  const img = cctx.getImageData(0, 0, W, paperH);
  const d = img.data;
  const n = W * paperH;
  const gray = new Float32Array(n);

  const bri = +$('brightness').value;
  const con = +$('contrast').value;
  const cf = (259 * (con + 255)) / (255 * (259 - con));
  const inv = $('invert').checked;

  for (let i = 0; i < n; i++) {
    // luminance, alpha over white
    const a = d[i * 4 + 3] / 255;
    let g = (0.299 * d[i * 4] + 0.587 * d[i * 4 + 1] + 0.114 * d[i * 4 + 2]) * a + 255 * (1 - a);
    g = cf * (g - 128) + 128 + bri;
    if (inv) g = 255 - g;
    gray[i] = Math.max(0, Math.min(255, g));
  }

  const mode = $('dither').value;
  const thr = +$('threshold').value;
  const cell = +$('cellSize').value;
  const bits = new Uint8Array(n);

  if (mode === 'threshold') {
    for (let i = 0; i < n; i++) bits[i] = gray[i] < thr ? 1 : 0;

  } else if (mode === 'bayer4' || mode === 'bayer8') {
    const m = mode === 'bayer4' ? BAYER4 : BAYER8;
    const s = mode === 'bayer4' ? 4 : 8;
    for (let y = 0; y < paperH; y++) for (let x = 0; x < W; x++) {
      const t = m[(y % s) * s + (x % s)] * 255;
      bits[y * W + x] = gray[y * W + x] < t + (thr - 128) ? 1 : 0;
    }

  } else if (mode === 'floyd' || mode === 'atkinson') {
    const g = Float32Array.from(gray);
    for (let y = 0; y < paperH; y++) for (let x = 0; x < W; x++) {
      const i = y * W + x;
      const old = g[i];
      const black = old < thr;
      bits[i] = black ? 1 : 0;
      const err = old - (black ? 0 : 255);
      if (mode === 'floyd') {
        if (x + 1 < W) g[i + 1] += err * 7 / 16;
        if (y + 1 < paperH) {
          if (x > 0) g[i + W - 1] += err * 3 / 16;
          g[i + W] += err * 5 / 16;
          if (x + 1 < W) g[i + W + 1] += err * 1 / 16;
        }
      } else {
        const e = err / 8;
        if (x + 1 < W) g[i + 1] += e;
        if (x + 2 < W) g[i + 2] += e;
        if (y + 1 < paperH) {
          if (x > 0) g[i + W - 1] += e;
          g[i + W] += e;
          if (x + 1 < W) g[i + W + 1] += e;
        }
        if (y + 2 < paperH) g[i + 2 * W] += e;
      }
    }

  } else if (mode === 'halftone') {
    // classic dot screen: per cell, average darkness -> dot radius
    for (let cy = 0; cy < paperH; cy += cell) for (let cx = 0; cx < W; cx += cell) {
      let sum = 0, cnt = 0;
      for (let y = cy; y < Math.min(cy + cell, paperH); y++)
        for (let x = cx; x < Math.min(cx + cell, W); x++) { sum += gray[y * W + x]; cnt++; }
      const darkness = 1 - (sum / cnt) / 255;                // 0..1
      let r = Math.sqrt(darkness) * cell * 0.72 * (thr / 128);
      // thermal heads can't burn single-pixel dots consistently: enforce a
      // minimum 2x2 dot, and drop dots too light to earn one
      if (r > 0 && r < 1) r = darkness * (thr / 128) > 0.08 ? 1 : 0;
      const mx = cx + cell / 2, my = cy + cell / 2;
      for (let y = cy; y < Math.min(cy + cell, paperH); y++)
        for (let x = cx; x < Math.min(cx + cell, W); x++) {
          const dx = x + .5 - mx, dy = y + .5 - my;
          if (dx * dx + dy * dy <= r * r) bits[y * W + x] = 1;
        }
    }

  } else if (mode === 'crosshatch') {
    // layered diagonal line screens; more layers where darker
    const p = cell;
    for (let y = 0; y < paperH; y++) for (let x = 0; x < W; x++) {
      const darkness = (255 - gray[y * W + x]) / 255 * (thr / 128);
      let on = false;
      if (darkness > 0.15) on ||= ((x + y) % p) === 0;
      if (darkness > 0.4)  on ||= ((x - y) % p + p) % p === 0;
      if (darkness > 0.65) on ||= (x % p) === 0;
      if (darkness > 0.85) on ||= (y % p) === 0;
      if (darkness > 0.97) on = true;
      bits[y * W + x] = on ? 1 : 0;
    }
  }
  return bits;
}

// ---------- preview ----------
let renderQueued = false;
function render() {
  if (renderQueued) return;
  renderQueued = true;
  // setTimeout, not requestAnimationFrame: rAF stalls in hidden/background tabs
  setTimeout(() => {
    renderQueued = false;
    preview.height = paperH;
    const bits = ditherToBits();
    const out = pctx.createImageData(W, paperH);
    for (let i = 0; i < bits.length; i++) {
      const v = bits[i] ? 20 : 250; // near-black on paper-white
      out.data[i * 4] = v; out.data[i * 4 + 1] = v; out.data[i * 4 + 2] = bits[i] ? 30 : 244;
      out.data[i * 4 + 3] = 255;
    }
    pctx.putImageData(out, 0, 0);
    drawSelection();
    $('previewMeta').textContent =
      `384 × ${paperH} px  ·  ~${(paperH / 203 * 2.54).toFixed(1)} cm of paper`;
    latestBits = bits;
  }, 0);
}
let latestBits = null;

function drawSelection() {
  overlay.innerHTML = '';
  if (!selected) return;
  const b = elBounds(selected);
  const box = document.createElement('div');
  box.className = 'selbox';
  box.style.cssText = `left:${b.x - 2}px; top:${b.y - 2}px; width:${b.w + 4}px; height:${b.h + 4}px;`;
  if (selected.type === 'image') {
    const h = document.createElement('div');
    h.className = 'handle';
    h.addEventListener('pointerdown', startResize);
    box.appendChild(h);
  }
  overlay.appendChild(box);
}

// ---------- UI wiring ----------
// Layer list: shown top-layer-first (like paint programs). elements[] is
// drawn in order, so index length-1 is the topmost layer = first list item.
function refreshElementList() {
  const list = $('elementList');
  list.innerHTML = '';
  for (let i = elements.length - 1; i >= 0; i--) {
    const el = elements[i];
    const item = document.createElement('div');
    item.className = 'elItem' + (el === selected ? ' selected' : '');
    item.draggable = true;
    item.dataset.index = i;
    const label = el.type === 'text'
      ? `📝 ${el.text.split('\n')[0].slice(0, 30) || '(empty)'}`
      : `🖼 ${el.name || 'image'}`;
    item.innerHTML = `<span class="grip">⠿</span><span class="name"></span><button class="del" title="Delete layer">✕</button>`;
    item.querySelector('.name').textContent = label;
    item.addEventListener('click', () => select(el));
    item.querySelector('.del').addEventListener('click', e => {
      e.stopPropagation();
      elements.splice(i, 1);
      if (selected === el) { selected = null; refreshPropsPanel(); }
      refreshElementList(); render();
    });

    item.addEventListener('dragstart', e => {
      e.dataTransfer.effectAllowed = 'move';
      e.dataTransfer.setData('text/plain', String(i));
      item.classList.add('dragging');
    });
    item.addEventListener('dragend', () => item.classList.remove('dragging'));
    const inUpperHalf = e => {
      const r = item.getBoundingClientRect();
      return e.clientY < r.top + r.height / 2;
    };
    item.addEventListener('dragover', e => {
      e.preventDefault();
      const above = inUpperHalf(e);
      item.classList.toggle('dropAbove', above);
      item.classList.toggle('dropBelow', !above);
    });
    item.addEventListener('dragleave', () => item.classList.remove('dropAbove', 'dropBelow'));
    item.addEventListener('drop', e => {
      e.preventDefault();
      item.classList.remove('dropAbove', 'dropBelow');
      const raw = e.dataTransfer.getData('text/plain');
      if (!/^\d+$/.test(raw)) return; // not one of our layer drags
      const from = +raw;
      if (from >= elements.length || from === i) return;
      // dropping above a list item = higher layer = larger array index
      const above = inUpperHalf(e);
      const moved = elements.splice(from, 1)[0];
      let to = i;
      if (from < i) to = above ? i : i - 1;
      else to = above ? i + 1 : i;
      elements.splice(Math.max(0, Math.min(elements.length, to)), 0, moved);
      refreshElementList(); render();
    });

    list.appendChild(item);
  }
}

function refreshPropsPanel() {
  $('elProps').hidden = !selected;
  if (!selected) return;
  $('textProps').hidden = selected.type !== 'text';
  $('imageProps').hidden = selected.type !== 'image';
  if (selected.type === 'text') {
    $('textContent').value = selected.text;
    $('textFont').value = selected.font;
    $('textAlign').value = selected.align;
    $('textBold').checked = selected.bold;
    $('textSize').value = selected.size;
    $('textSize').nextElementSibling.value = selected.size;
  } else {
    const pct = Math.round(selected.w / selected.img.width * 100);
    $('imgScale').value = pct;
    $('imgScale').nextElementSibling.value = pct;
  }
}

$('addText').addEventListener('click', addTextEl);
$('addImage').addEventListener('click', () => $('imageFile').click());
$('imageFile').addEventListener('change', e => {
  const f = e.target.files[0];
  if (!f) return;
  const img = new Image();
  img.onload = () => { addImageEl(img, f.name); URL.revokeObjectURL(img.src); };
  img.src = URL.createObjectURL(f);
  e.target.value = '';
});
$('deleteEl').addEventListener('click', deleteSelected);

$('textContent').addEventListener('input', e => { if (selected) { selected.text = e.target.value; refreshElementList(); render(); } });
$('textFont').addEventListener('change', e => { if (selected) { selected.font = e.target.value; render(); } });
$('textAlign').addEventListener('change', e => { if (selected) { selected.align = e.target.value; render(); } });
$('textBold').addEventListener('change', e => { if (selected) { selected.bold = e.target.checked; render(); } });
$('textSize').addEventListener('input', e => {
  e.target.nextElementSibling.value = e.target.value;
  if (selected) { selected.size = +e.target.value; render(); }
});
$('imgScale').addEventListener('input', e => {
  e.target.nextElementSibling.value = e.target.value;
  if (selected && selected.type === 'image') {
    selected.w = Math.max(4, Math.round(selected.img.width * e.target.value / 100));
    selected.h = Math.max(4, Math.round(selected.w / selected.aspect));
    render();
  }
});

for (const id of ['dither', 'brightness', 'contrast', 'threshold', 'cellSize', 'invert']) {
  $(id).addEventListener('input', e => {
    if (e.target.nextElementSibling?.tagName === 'OUTPUT')
      e.target.nextElementSibling.value = e.target.value;
    $('cellRow').style.display =
      ['halftone', 'crosshatch', 'bayer4', 'bayer8'].includes($('dither').value) ? '' : 'none';
    render();
  });
}
$('cellRow').style.display = 'none';

function setPaperH(h) {
  paperH = Math.max(100, Math.min(2400, Math.round(h / 8) * 8));
  render();
}
$('fitHeight').addEventListener('click', () => {
  let maxY = 100;
  for (const el of elements) { const b = elBounds(el); maxY = Math.max(maxY, b.y + b.h); }
  setPaperH(maxY + 16);
});

// drag the grip below the paper to change its length
$('paperGrip').addEventListener('pointerdown', e => {
  e.preventDefault();
  $('paperGrip').setPointerCapture(e.pointerId);
  const startH = paperH, sy = e.clientY;
  const move = ev => setPaperH(startH + (ev.clientY - sy));
  const up = ev => {
    $('paperGrip').releasePointerCapture(e.pointerId);
    $('paperGrip').removeEventListener('pointermove', move);
    $('paperGrip').removeEventListener('pointerup', up);
  };
  $('paperGrip').addEventListener('pointermove', move);
  $('paperGrip').addEventListener('pointerup', up);
});

// ---------- drag / resize on the paper ----------
const paper = document.getElementById('paper');
let drag = null;

paper.addEventListener('pointerdown', e => {
  if (e.target.classList.contains('handle')) return;
  const r = paper.getBoundingClientRect();
  const x = e.clientX - r.left, y = e.clientY - r.top;
  // topmost element under cursor
  let hit = null;
  for (let i = elements.length - 1; i >= 0; i--) {
    const b = elBounds(elements[i]);
    if (x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h) { hit = elements[i]; break; }
  }
  select(hit);
  if (hit) {
    drag = { el: hit, dx: x - hit.x, dy: y - hit.y };
    paper.setPointerCapture(e.pointerId);
  }
});
paper.addEventListener('pointermove', e => {
  if (!drag) return;
  const r = paper.getBoundingClientRect();
  drag.el.x = Math.round(e.clientX - r.left - drag.dx);
  drag.el.y = Math.round(e.clientY - r.top - drag.dy);
  render();
});
paper.addEventListener('pointerup', () => { drag = null; });

function startResize(e) {
  e.stopPropagation();
  const el = selected;
  if (!el || el.type !== 'image') return;
  const startW = el.w, sx = e.clientX;
  const move = ev => {
    el.w = Math.max(8, Math.round(startW + (ev.clientX - sx)));
    el.h = Math.max(4, Math.round(el.w / el.aspect));
    refreshPropsPanel(); render();
  };
  const up = () => { window.removeEventListener('pointermove', move); window.removeEventListener('pointerup', up); };
  window.addEventListener('pointermove', move);
  window.addEventListener('pointerup', up);
}

window.addEventListener('keydown', e => {
  if ((e.key === 'Delete' || e.key === 'Backspace') && selected &&
      !['TEXTAREA', 'INPUT'].includes(document.activeElement.tagName)) {
    deleteSelected();
  }
});

// ---------- printing ----------
function packBits(bits) {
  const bytes = new Uint8Array((W / 8) * paperH);
  for (let i = 0; i < bits.length; i++)
    if (bits[i]) bytes[i >> 3] |= 0x80 >> (i & 7);
  let bin = '';
  for (let i = 0; i < bytes.length; i += 0x8000)
    bin += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
  return btoa(bin);
}

const msg = $('msg');
function setMsg(text, cls = '') { msg.textContent = text; msg.className = cls; }

$('printBtn').addEventListener('click', async () => {
  if (!latestBits) return;
  const btn = $('printBtn');
  btn.disabled = true;
  setMsg('Sending…');
  try {
    const res = await fetch(`${RELAY_URL}/api/print`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ width: W, height: paperH, data: packBits(latestBits) }),
    });
    const body = await res.json().catch(() => ({}));
    if (!res.ok) throw new Error(body.error || `HTTP ${res.status}`);
    setMsg('Queued! Watching printer…', 'ok');
    await watchJob(body.id);
  } catch (err) {
    setMsg(`✗ ${err.message}`, 'err');
  } finally {
    btn.disabled = false;
  }
});

async function watchJob(id) {
  for (let i = 0; i < 60; i++) {
    await new Promise(r => setTimeout(r, 2000));
    const res = await fetch(`${RELAY_URL}/api/job/${id}`).catch(() => null);
    if (!res || !res.ok) continue;
    const { state } = await res.json();
    if (state === 'done') { setMsg('✓ Printed! It is now physically on paper.', 'ok'); return; }
    if (state.startsWith('error')) { setMsg(`✗ ${state}`, 'err'); return; }
    setMsg(state === 'printing' ? 'Printing…' : 'Queued…');
  }
  setMsg('Lost track of the job — check the printer.', 'err');
}

// ---------- status pill ----------
async function pollStatus() {
  const el = $('status');
  try {
    const res = await fetch(`${RELAY_URL}/api/status`);
    const s = await res.json();
    const on = s.bridgeOnline && s.printerOnline;
    el.innerHTML = `<span class="dot ${on ? 'on' : 'off'}"></span>` +
      (on ? 'printer online' : s.bridgeOnline ? 'bridge up, printer off' : 'printer offline');
  } catch {
    el.innerHTML = '<span class="dot off"></span>relay unreachable';
  }
}
pollStatus();
setInterval(pollStatus, 15000);

// ---------- init ----------
addTextEl();
selected.text = 'Hello from evankoza.com';
refreshPropsPanel(); refreshElementList();
render();
