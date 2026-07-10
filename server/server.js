// Thermal print relay.
//
//  Browser --HTTPS POST /api/print--> [this server] --WebSocket--> ESP32 --BT--> printer
//
// The ESP32 connects OUT to ws(s)://host/device?token=..., so the ESP32 never
// needs a port open. Jobs are queued and sent one at a time; the ESP32 replies
// {"type":"done"|"error","id":N} per job.

import express from 'express';
import http from 'http';
import { WebSocketServer } from 'ws';
import crypto from 'crypto';
import path from 'path';
import fs from 'fs';
import { fileURLToPath } from 'url';

const PORT = process.env.PORT || 8377;
const DEVICE_TOKEN = process.env.DEVICE_TOKEN || 'CHANGE_ME_LONG_RANDOM_STRING';
const ALLOWED_ORIGINS = (process.env.ALLOWED_ORIGINS ||
  'https://evankoza.com,https://www.evankoza.com,http://localhost:8000,http://127.0.0.1:8000'
).split(',').map(s => s.trim());

const WIDTH_DOTS = 384;
const WIDTH_BYTES = WIDTH_DOTS / 8;
const MAX_HEIGHT = 400;           // rows (~5 cm of paper — public page, save the roll)
const MAX_QUEUE = 50;               // the printer PC can be off for hours — hold a real backlog
const QUEUE_FILE = process.env.QUEUE_FILE || ''; // set to persist the queue across restarts
const JOB_TIMEOUT_MS = 180_000; // max-length job + slow-burn pauses can near 2 min
const RATE_LIMIT = { windowMs: 60_000, max: 3 }; // per IP: 3 prints/minute

const app = express();
app.set('trust proxy', true); // behind nginx/caddy
app.use(express.json({ limit: '1mb' }));

app.use((req, res, next) => {
  const origin = req.headers.origin;
  if (origin && ALLOWED_ORIGINS.includes(origin)) {
    res.setHeader('Access-Control-Allow-Origin', origin);
    res.setHeader('Access-Control-Allow-Methods', 'GET,POST,OPTIONS');
    res.setHeader('Access-Control-Allow-Headers', 'Content-Type');
  }
  if (req.method === 'OPTIONS') return res.sendStatus(204);
  next();
});

// Serve the editor itself if web/ is present next to server/ — handy for
// tunnels and for running the whole demo off one process. Same-origin, so
// the browser needs no CORS for this case.
const webDir = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', 'web');
if (fs.existsSync(webDir)) app.use(express.static(webDir));

// ---- rate limiting (in-memory, fine for a hobby relay) ----
const hits = new Map(); // ip -> [timestamps]
function rateLimited(ip) {
  const now = Date.now();
  const arr = (hits.get(ip) || []).filter(t => now - t < RATE_LIMIT.windowMs);
  if (arr.length >= RATE_LIMIT.max) { hits.set(ip, arr); return true; }
  arr.push(now);
  hits.set(ip, arr);
  return false;
}
setInterval(() => { // prune
  const now = Date.now();
  for (const [ip, arr] of hits) {
    const keep = arr.filter(t => now - t < RATE_LIMIT.windowMs);
    if (keep.length) hits.set(ip, keep); else hits.delete(ip);
  }
}, 60_000).unref();

// ---- job queue / device link ----
let device = null;            // ws connection from ESP32
let deviceStatus = { printer: false };
let queue = [];               // {id, buf, res-resolved-already, state}
let active = null;            // job currently at the printer
let nextId = 1;
const jobStates = new Map();  // id -> 'queued'|'printing'|'done'|'error:msg'

// ---- queue persistence ----
// The PC that owns the printer sleeps for hours at a time; jobs wait here
// meanwhile, so they must also survive relay restarts. The active job is
// snapshotted too — after a crash mid-print a duplicate beats a lost print.
function saveQueue() {
  if (!QUEUE_FILE) return;
  const jobs = (active ? [active, ...queue] : queue)
    .map(j => ({ id: j.id, height: j.height, bits: j.bits.toString('base64') }));
  try {
    fs.writeFileSync(QUEUE_FILE + '.tmp', JSON.stringify({ nextId, jobs }));
    fs.renameSync(QUEUE_FILE + '.tmp', QUEUE_FILE);
  } catch (e) { console.error('queue save failed:', e.message); }
}
function loadQueue() {
  if (!QUEUE_FILE || !fs.existsSync(QUEUE_FILE)) return;
  try {
    const saved = JSON.parse(fs.readFileSync(QUEUE_FILE, 'utf8'));
    nextId = saved.nextId || 1;
    queue = saved.jobs.map(j => ({ id: j.id, height: j.height, bits: Buffer.from(j.bits, 'base64') }));
    for (const j of queue) jobStates.set(j.id, 'queued');
    if (queue.length) console.log(`restored ${queue.length} queued job(s)`);
  } catch (e) { console.error('queue load failed:', e.message); }
}
loadQueue();

// Jobs are streamed as stripe-sized binary chunks (the ESP32's websocket
// library chokes on large single frames). Each chunk reuses the same header
// (widthBytes, rows-in-chunk, jobId); a {"type":"jobEnd"} text frame follows,
// which the ESP32 acks with done/error after feeding the paper.
const CHUNK_ROWS = 64;

function pump() {
  // hold jobs until the printer is actually reachable — dispatching while the
  // BT link is down just makes the ESP32 fail the job ("printer offline")
  if (active || !device || device.readyState !== 1 || !deviceStatus.printer ||
      queue.length === 0) return;
  active = queue.shift();
  jobStates.set(active.id, 'printing');
  for (let y = 0; y < active.height; y += CHUNK_ROWS) {
    const rows = Math.min(CHUNK_ROWS, active.height - y);
    const chunk = Buffer.alloc(8 + rows * WIDTH_BYTES);
    chunk.writeUInt16LE(WIDTH_BYTES, 0);
    chunk.writeUInt16LE(rows, 2);
    chunk.writeUInt32LE(active.id, 4);
    active.bits.copy(chunk, 8, y * WIDTH_BYTES, (y + rows) * WIDTH_BYTES);
    device.send(chunk, { binary: true });
  }
  device.send(JSON.stringify({ type: 'jobEnd', id: active.id }));
  active.timer = setTimeout(() => {
    jobStates.set(active.id, 'error: timeout');
    active = null;
    saveQueue();
    pump();
  }, JOB_TIMEOUT_MS);
}

function finishActive(ok, message) {
  if (!active) return;
  clearTimeout(active.timer);
  jobStates.set(active.id, ok ? 'done' : `error: ${message || 'print failed'}`);
  active = null;
  saveQueue();
  pump();
}

// ---- HTTP API ----
app.get('/api/status', (req, res) => {
  res.json({
    bridgeOnline: !!device && device.readyState === 1,
    printerOnline: !!deviceStatus.printer,
    queueLength: queue.length + (active ? 1 : 0),
  });
});

app.get('/api/job/:id', (req, res) => {
  const state = jobStates.get(Number(req.params.id));
  if (!state) return res.status(404).json({ error: 'unknown job' });
  res.json({ state });
});

app.post('/api/print', (req, res) => {
  const ip = req.ip;
  if (rateLimited(ip)) return res.status(429).json({ error: 'Rate limit: 3 prints per minute. Chill :)' });

  const { width, height, data } = req.body || {};
  if (width !== WIDTH_DOTS) return res.status(400).json({ error: `width must be ${WIDTH_DOTS}` });
  if (!Number.isInteger(height) || height < 1 || height > MAX_HEIGHT)
    return res.status(400).json({ error: `height must be 1..${MAX_HEIGHT}` });

  let bits;
  try { bits = Buffer.from(data, 'base64'); } catch { bits = null; }
  if (!bits || bits.length !== WIDTH_BYTES * height)
    return res.status(400).json({ error: 'data length mismatch' });

  // bridge/printer offline is fine — jobs wait in the queue and print on return
  if (queue.length >= MAX_QUEUE)
    return res.status(503).json({ error: 'Queue full, try again shortly' });

  const id = nextId++;
  queue.push({ id, bits, height });
  jobStates.set(id, 'queued');
  if (jobStates.size > 200) { // prune old job records
    for (const k of jobStates.keys()) { if (jobStates.size <= 100) break; jobStates.delete(k); }
  }
  console.log(`[job ${id}] queued from ${ip}, ${height} rows`);
  saveQueue();
  pump();
  res.json({ id, queued: true });
});

// ---- WebSocket endpoint for the ESP32 ----
const server = http.createServer(app);
const wss = new WebSocketServer({ server, path: '/device' });

wss.on('connection', (ws, req) => {
  const url = new URL(req.url, 'http://x');
  const token = url.searchParams.get('token') || '';
  const ok = token.length === DEVICE_TOKEN.length &&
    crypto.timingSafeEqual(Buffer.from(token), Buffer.from(DEVICE_TOKEN));
  if (!ok) { ws.close(4001, 'bad token'); return; }

  console.log('ESP32 bridge connected');
  if (device && device.readyState === 1) device.close();
  device = ws;

  ws.on('message', (msg, isBinary) => {
    if (isBinary) return;
    let m;
    try { m = JSON.parse(msg.toString()); } catch { return; }
    if (m.type === 'status') { deviceStatus = m; pump(); } // printer may have just come back
    else if (m.type === 'done' && active && m.id === active.id) finishActive(true);
    else if (m.type === 'error' && active && m.id === active.id) finishActive(false, m.message);
    else if (m.type === 'requeue' && active && m.id === active.id) {
      // the printer vanished under the bridge mid-job: hold the job for its
      // return instead of guessing it printed. Cap reprints in case a dying
      // battery makes it flap mid-job forever.
      clearTimeout(active.timer);
      const job = active;
      active = null;
      deviceStatus.printer = false; // don't redispatch until a status says otherwise
      job.requeues = (job.requeues || 0) + 1;
      if (job.requeues > 5) {
        jobStates.set(job.id, 'error: printer kept dropping mid-print');
      } else {
        jobStates.set(job.id, 'queued');
        queue.unshift(job);
        console.log(`[job ${job.id}] printer offline at the bridge, requeued`);
      }
      saveQueue();
    }
  });

  ws.on('close', () => {
    if (device === ws) { device = null; deviceStatus = { printer: false }; }
    // the in-flight job's ack is lost with the socket: retry it once, then fail
    if (active) {
      clearTimeout(active.timer);
      const job = active;
      active = null;
      if ((job.attempts || 0) < 1) {
        job.attempts = (job.attempts || 0) + 1;
        jobStates.set(job.id, 'queued');
        queue.unshift(job);
        console.log(`[job ${job.id}] bridge dropped mid-job, requeued`);
      } else {
        jobStates.set(job.id, 'error: bridge disconnected');
      }
      saveQueue();
    }
    console.log('ESP32 bridge disconnected');
  });

  pump();
});

server.listen(PORT, () => console.log(`Print relay listening on :${PORT}`));
