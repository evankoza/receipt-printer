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

// Write to BT in small chunks, yielding so WiFi/WS stay alive.
static bool btWriteAll(const uint8_t* data, size_t len) {
  size_t sent = 0;
  while (sent < len) {
    if (!bt.connected()) return false;
    size_t n = min((size_t)BT_CHUNK, len - sent);
    size_t w = bt.write(data + sent, n);
    if (w == 0) { delay(20); continue; }
    sent += w;
    delay(2); // pace SPP; printer buffer is small
  }
  return true;
}

static bool printBitmap(const uint8_t* bits, uint16_t widthBytes, uint16_t height) {
  // Send in stripes: GS v 0 m=0, xL xH yL yH, data
  for (uint16_t y = 0; y < height; y += STRIPE_ROWS) {
    uint16_t rows = min((uint16_t)STRIPE_ROWS, (uint16_t)(height - y));
    uint8_t hdr[8] = {
      0x1D, 0x76, 0x30, 0x00,
      (uint8_t)(widthBytes & 0xFF), (uint8_t)(widthBytes >> 8),
      (uint8_t)(rows & 0xFF), (uint8_t)(rows >> 8)
    };
    if (!btWriteAll(hdr, sizeof(hdr))) return false;
    if (!btWriteAll(bits + (size_t)y * widthBytes, (size_t)rows * widthBytes)) return false;
  }
  return true;
}

static bool feedPaper() {
  uint8_t feed[] = {0x1B, 0x64, FEED_LINES_AFTER}; // ESC d n
  return btWriteAll(feed, sizeof(feed));
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
  Serial.printf("Printing job %u chunk (%u rows)\n", jobId, rows);
  lastChunkMs = millis();
  currentJobId = jobId;
  if (!printBitmap(payload + 8, widthBytes, rows)) {
    printerConnected = false;
    failJob(jobId, "bluetooth write failed");
  }
}

static void handleJobEnd(uint32_t jobId) {
  JsonDocument reply;
  reply["id"] = jobId;
  if (jobId == failedJobId) {
    reply["type"] = "error";
    reply["message"] = failReason;
    jobsFailed++;
  } else {
    feedPaper();
    reply["type"] = "done";
    jobsDone++;
  }
  lastChunkMs = 0;
  sendJson(reply);
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

  bt.begin("ESP32-PrintBridge", true); // true = master (we connect out)
  bt.setPin(PRINTER_PIN);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(50); displayBoot("joining " WIFI_SSID);
    static uint32_t lastDot = 0;
    if (millis() - lastDot > 300) { lastDot = millis(); Serial.print("."); }
  }
  Serial.printf("\nWiFi: %s\n", WiFi.localIP().toString().c_str());

  String path = String(RELAY_PATH) + "?token=" + DEVICE_TOKEN;
  if (RELAY_USE_TLS) ws.beginSSL(RELAY_HOST, RELAY_PORT, path);
  else               ws.begin(RELAY_HOST, RELAY_PORT, path);
  ws.onEvent(wsEvent);
  ws.setReconnectInterval(5000);
  ws.enableHeartbeat(15000, 5000, 3);
}

void loop() {
  ws.loop();

  printerConnected = bt.connected();
  if (!printerConnected && millis() - lastPrinterAttempt > 15000) {
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
