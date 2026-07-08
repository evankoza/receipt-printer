// ESP32 print bridge: WebSocket client to the relay <-> Bluetooth SPP to MPT-II.
//
// Job protocol: the relay streams a job as several small binary chunks
// (large single frames break the websocket library). Each chunk:
//   [0..1]  uint16 LE  widthBytes (must be 48)
//   [2..3]  uint16 LE  rows in this chunk
//   [4..7]  uint32 LE  jobId
//   [8.. ]  widthBytes * rows bytes, 1 bit per dot, MSB = leftmost, 1 = black
// Chunks are printed immediately. A text frame {"type":"jobEnd","id":N}
// follows the last chunk; we feed the paper and ack with
// {"type":"done"|"error","id":N,...}.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_bt.h>
#include <BluetoothSerial.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include "config.h"
#include "display.h"

BluetoothSerial bt;
WebSocketsClient ws;

static bool printerConnected = false;
static uint32_t lastPrinterAttempt = 0;
static uint32_t lastStatusSent = 0;

// dashboard counters
static uint32_t jobsDone = 0;
static uint32_t jobsFailed = 0;
static uint32_t lastChunkMs = 0;   // chunks flowing -> "printing" on the OLED
static uint32_t currentJobId = 0;

// ---------- Bluetooth / printer ----------

static void scanBtDevices() {
  Serial.println("Scanning for Bluetooth devices (10s)...");
  BTScanResults* results = bt.discover(10000);
  if (!results) { Serial.println("Scan failed"); return; }
  for (int i = 0; i < results->getCount(); i++) {
    BTAdvertisedDevice* d = results->getDevice(i);
    Serial.printf("  [%s] %s\n", d->getAddress().toString().c_str(), d->getName().c_str());
  }
}

static bool connectPrinter() {
  if (strlen(PRINTER_NAME) == 0) { scanBtDevices(); return false; }
  Serial.printf("Connecting to printer '%s'...\n", PRINTER_NAME);
  if (bt.connect(PRINTER_NAME)) {
    Serial.println("Printer connected");
    // ESC @ : initialize
    uint8_t init[] = {0x1B, 0x40};
    bt.write(init, sizeof(init));
    return true;
  }
  Serial.println("Printer connect failed");
  return false;
}

// ---------- job buffer ----------
// Chunks are buffered here and drained to BT from loop(), a little per pass.
// Printing inline in the WS callback blocks ws.loop() for the whole job
// (SPP drains at printer speed), the heartbeat misses and the relay drops
// mid-print — which over the TLS funnel costs a reboot and a requeue loop.
#define JOB_BUF_ROWS 420  // relay caps jobs at 400 rows
// Allocated on the heap AFTER the BT stack is up: as a static array it shrank
// the heap enough that BT controller init null-crashed (StoreProhibited).
static uint8_t* jobBuf = nullptr;
static size_t jobLen = 0;       // bytes buffered
static size_t jobSent = 0;      // bytes already handed to BT (incl. headers logic below)
static uint16_t rowsBuffered = 0;
static uint16_t rowsSent = 0;   // rows fully dispatched to BT
static bool jobEndSeen = false;
static uint32_t jobEndId = 0;

// One bounded BT write; returns bytes accepted (0 = queue full, try later).
static size_t btTryWrite(const uint8_t* data, size_t len) {
  if (!bt.connected()) return 0;
  return bt.write(data, min(len, (size_t)BT_CHUNK));
}

// Write a small buffer, retrying briefly (headers/feeds only — a few bytes).
static bool btWriteAll(const uint8_t* data, size_t len) {
  size_t sent = 0;
  for (int tries = 0; sent < len && tries < 200; tries++) {
    if (!bt.connected()) return false;
    size_t w = bt.write(data + sent, len - sent);
    if (w == 0) delay(10); else sent += w;
  }
  return sent == len;
}

static bool feedPaper() {
  // This printer ignores ESC d (feed n lines) just like it ignores the
  // darkness commands — feed with blank raster rows via GS v 0 instead,
  // the one command it demonstrably honors.
  const uint16_t rows = FEED_RASTER_ROWS;
  uint8_t hdr[8] = {
    0x1D, 0x76, 0x30, 0x00,
    (uint8_t)(PRINTER_WIDTH_BYTES & 0xFF), (uint8_t)(PRINTER_WIDTH_BYTES >> 8),
    (uint8_t)(rows & 0xFF), (uint8_t)(rows >> 8)
  };
  if (!btWriteAll(hdr, sizeof(hdr))) return false;
  uint8_t zeros[PRINTER_WIDTH_BYTES] = {0};
  for (uint16_t y = 0; y < rows; y++)
    if (!btWriteAll(zeros, sizeof(zeros))) return false;
  return true;
}

// ---------- WebSocket ----------

static void sendJson(JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  ws.sendTXT(out);
}

static void sendStatus() {
  JsonDocument doc;
  doc["type"] = "status";
  doc["printer"] = printerConnected;
  doc["heap"] = ESP.getFreeHeap();
  sendJson(doc);
}

static uint32_t failedJobId = 0;  // ignore remaining chunks of a job that errored
static const char* failReason = "";

static void failJob(uint32_t jobId, const char* why) {
  failedJobId = jobId;
  failReason = why;
}

// Buffer the chunk; actual BT output happens in drainJob() from loop().
static void handleChunk(uint8_t* payload, size_t length) {
  if (length < 8) return;
  uint16_t widthBytes = payload[0] | (payload[1] << 8);
  uint16_t rows       = payload[2] | (payload[3] << 8);
  uint32_t jobId      = payload[4] | (payload[5] << 8) | (payload[6] << 16) | ((uint32_t)payload[7] << 24);

  if (jobId == failedJobId) return; // job already failed, drain its chunks

  if (widthBytes != PRINTER_WIDTH_BYTES || rows == 0 ||
      length - 8 != (size_t)widthBytes * rows) {
    failJob(jobId, "bad chunk dimensions");
    return;
  }
  if (!printerConnected || !bt.connected()) {
    failJob(jobId, "printer offline");
    return;
  }
  if (jobId != currentJobId) { // new job starts
    currentJobId = jobId;
    jobLen = jobSent = 0; rowsBuffered = rowsSent = 0; jobEndSeen = false;
  }
  if (!jobBuf || rowsBuffered + rows > JOB_BUF_ROWS) {
    failJob(jobId, !jobBuf ? "no job buffer" : "job too tall for buffer");
    return;
  }
  Serial.printf("Buffering job %u chunk (%u rows)\n", jobId, rows);
  memcpy(jobBuf + jobLen, payload + 8, (size_t)widthBytes * rows);
  jobLen += (size_t)widthBytes * rows;
  rowsBuffered += rows;
  lastChunkMs = millis();
}

static void finishJob(uint32_t jobId, bool ok, const char* why) {
  JsonDocument reply;
  reply["id"] = jobId;
  if (ok) { reply["type"] = "done"; jobsDone++; }
  else    { reply["type"] = "error"; reply["message"] = why; jobsFailed++; }
  lastChunkMs = 0;
  jobLen = jobSent = 0; rowsBuffered = rowsSent = 0; jobEndSeen = false;
  currentJobId = 0;
  if (ws.isConnected()) sendJson(reply);
}

static void handleJobEnd(uint32_t jobId) {
  if (jobId == failedJobId) { finishJob(jobId, false, failReason); return; }
  jobEndSeen = true;
  jobEndId = jobId;   // finishJob happens in drainJob() once all rows are out
}

// Called every loop() pass: push at most one stripe header + pending bytes
// to BT without ever busy-waiting, so ws.loop() keeps the connection alive.
static void drainJob() {
  if (jobLen == 0 || rowsSent >= rowsBuffered) {
    if (jobEndSeen && jobLen > 0 && rowsSent >= rowsBuffered) {
      bool ok = feedPaper();
      finishJob(jobEndId, ok, "bluetooth write failed");
    }
    return;
  }
  if (!bt.connected()) {
    printerConnected = false;
    failJob(currentJobId, "bluetooth write failed");
    finishJob(currentJobId, false, failReason);
    return;
  }

  // stripe bookkeeping: emit a GS v 0 header at each stripe boundary
  static uint16_t stripeRowsLeft = 0;   // rows remaining in the open stripe
  static size_t   stripeBytesLeft = 0;  // data bytes remaining in the open stripe

  if (stripeBytesLeft == 0) {
    if (rowsSent >= rowsBuffered) return;
    uint16_t rows = min((uint16_t)STRIPE_ROWS, (uint16_t)(rowsBuffered - rowsSent));
    // don't start a stripe we don't have full data for unless jobEnd arrived
    if (!jobEndSeen && rows < STRIPE_ROWS) return;
    uint8_t hdr[8] = {
      0x1D, 0x76, 0x30, 0x00,
      (uint8_t)(PRINTER_WIDTH_BYTES & 0xFF), (uint8_t)(PRINTER_WIDTH_BYTES >> 8),
      (uint8_t)(rows & 0xFF), (uint8_t)(rows >> 8)
    };
    if (!btWriteAll(hdr, sizeof(hdr))) return; // retry next pass
    stripeRowsLeft = rows;
    stripeBytesLeft = (size_t)rows * PRINTER_WIDTH_BYTES;
    lastChunkMs = millis();
  }

  size_t w = btTryWrite(jobBuf + jobSent, stripeBytesLeft);
  if (w > 0) {
    jobSent += w;
    stripeBytesLeft -= w;
    lastChunkMs = millis();
    if (stripeBytesLeft == 0) { rowsSent += stripeRowsLeft; stripeRowsLeft = 0; }
  }
}

static void wsEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.println("Relay connected");
      sendStatus();
      break;
    case WStype_DISCONNECTED:
      Serial.println("Relay disconnected");
      break;
    case WStype_BIN:
      handleChunk(payload, length);
      break;
    case WStype_TEXT: {
      JsonDocument doc;
      if (deserializeJson(doc, payload, length) == DeserializationError::Ok) {
        const char* t = doc["type"] | "";
        if (strcmp(t, "jobEnd") == 0) { handleJobEnd(doc["id"] | 0); break; }
      }
      sendStatus();
      break;
    }
    default:
      break;
  }
}

// ---------- Setup / loop ----------

void setup() {
  Serial.begin(115200);
  delay(500);

  displayInit();

  // BT Classic only — release the BLE half of the controller RAM (~30 KB).
  esp_bt_controller_mem_release(ESP_BT_MODE_BLE);

  // NOTE: Bluetooth is NOT started here. BT Classic + the mbedTLS handshake
  // don't fit in RAM together (BIGNUM OOM -16), so we connect the WSS relay
  // first and only then start BT (see loop). If the relay drops later, the
  // re-handshake would OOM with BT running — the loop reboots instead.

  WiFi.mode(WIFI_STA);
  // Battery-bank friendly: cap radio TX power (full-power join bursts spike
  // ~500 mA and brown out weak USB sources — the "stuck joining" symptom).
  // Do NOT WiFi.setSleep(false): BT Classic shares the radio and its
  // controller abort()s at init without WiFi modem sleep (coexistence).
  WiFi.setTxPower(WIFI_POWER_11dBm);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("WiFi connecting");
  uint32_t wifiStart = millis(), lastKick = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(50); displayBoot("joining " WIFI_SSID);
    static uint32_t lastDot = 0;
    if (millis() - lastDot > 300) { lastDot = millis(); Serial.print("."); }
    // The join sometimes wedges (dot loop) — kick a fresh attempt every 15 s
    // and hard-reboot after 90 s rather than hang forever.
    if (millis() - lastKick > 15000) {
      lastKick = millis();
      Serial.print("(retry)");
      WiFi.disconnect(true);
      delay(200);
      WiFi.mode(WIFI_STA);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
    if (millis() - wifiStart > 90000) { Serial.println("\nWiFi stuck 90s, rebooting"); ESP.restart(); }
  }
  Serial.printf("\nWiFi: %s\n", WiFi.localIP().toString().c_str());

  String path = String(RELAY_PATH) + "?token=" + DEVICE_TOKEN;
  if (RELAY_USE_TLS) ws.beginSSL(RELAY_HOST, RELAY_PORT, path);
  else               ws.begin(RELAY_HOST, RELAY_PORT, path);
  ws.onEvent(wsEvent);
  ws.setReconnectInterval(5000);
  // Generous pong timeout: the Tailscale Funnel path can be slow and we'd
  // rather tolerate latency than drop (a drop costs a full reboot, see loop).
  ws.enableHeartbeat(25000, 12000, 5);
}

static bool btUp = false;
static uint32_t relayDownSince = 0;

void loop() {
  ws.loop();

  // Start Bluetooth only after the TLS websocket is established (see setup).
  if (!btUp && ws.isConnected()) {
    Serial.printf("Relay up, starting Bluetooth (heap %u)\n", ESP.getFreeHeap());
    bt.begin("ESP32-PrintBridge", true); // true = master (we connect out)
    bt.setPin(PRINTER_PIN);
    btUp = true;
    jobBuf = (uint8_t*)malloc((size_t)JOB_BUF_ROWS * PRINTER_WIDTH_BYTES);
    Serial.printf("BT up, job buffer %s (heap %u)\n", jobBuf ? "ok" : "ALLOC FAILED", ESP.getFreeHeap());
  }

  // With BT running the TLS re-handshake can't allocate — reboot to recover.
  if (btUp) {
    if (ws.isConnected()) relayDownSince = 0;
    else if (!relayDownSince) relayDownSince = millis();
    else if (millis() - relayDownSince > 20000) {
      Serial.println("Relay down >20s with BT active, rebooting");
      ESP.restart();
    }
  }

  printerConnected = btUp && bt.connected();
  if (printerConnected) drainJob();
  if (btUp && !printerConnected && millis() - lastPrinterAttempt > 15000) {
    lastPrinterAttempt = millis();
    printerConnected = connectPrinter();
  }

  if (millis() - lastStatusSent > 10000) {
    lastStatusSent = millis();
    if (ws.isConnected()) sendStatus();
  }

  static String ipStr;
  ipStr = WiFi.localIP().toString();
  BridgeState st;
  st.wifiUp = WiFi.status() == WL_CONNECTED;
  st.ip = ipStr.c_str();
  st.relayUp = ws.isConnected();
  st.printerUp = printerConnected;
  st.jobsDone = jobsDone;
  st.jobsFailed = jobsFailed;
  st.printing = lastChunkMs && millis() - lastChunkMs < 3000;
  st.printingJob = currentJobId;
  displayTick(st);
}
