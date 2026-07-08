// Simulates the ESP32 bridge: connects to the relay, reports printer online,
// consumes chunked jobs and acks on jobEnd. Run with the relay's DEVICE_TOKEN
// set to "test": DEVICE_TOKEN=test node server.js
import WebSocket from 'ws';
const ws = new WebSocket('ws://localhost:8377/device?token=test');
const rows = new Map(); // jobId -> row count so far
ws.on('open', () => {
  console.log('fake-esp32: connected');
  ws.send(JSON.stringify({ type: 'status', printer: true, heap: 123456 }));
});
ws.on('message', (msg, isBinary) => {
  if (isBinary) {
    const widthBytes = msg.readUInt16LE(0);
    const r = msg.readUInt16LE(2);
    const id = msg.readUInt32LE(4);
    const ok = widthBytes === 48 && msg.length - 8 === widthBytes * r;
    if (!ok) console.log(`fake-esp32: job ${id} BAD chunk (${widthBytes}x${r}, ${msg.length}b)`);
    rows.set(id, (rows.get(id) || 0) + r);
    return;
  }
  const m = JSON.parse(msg.toString());
  if (m.type === 'jobEnd') {
    console.log(`fake-esp32: job ${m.id} complete, ${rows.get(m.id) || 0} rows total`);
    setTimeout(() => ws.send(JSON.stringify({ type: 'done', id: m.id })), 300);
  }
});
ws.on('close', (c, r) => console.log('fake-esp32: closed', c, r.toString()));
