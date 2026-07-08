#include "display.h"
#include <TFT_eSPI.h>

static TFT_eSPI tft;

// palette (RGB565) — matches the evankoza.com look: pumpkin on near-black
static const uint16_t C_BG     = TFT_BLACK;
static const uint16_t C_PANEL  = 0x18E3;   // #1c1b18-ish dark panel
static const uint16_t C_TEXT   = 0xEF5C;   // #ECEAE3 cream
static const uint16_t C_MUTED  = 0x8410;   // grey
static const uint16_t C_ACCENT = 0xCA80;   // #C95000 pumpkin
static const uint16_t C_OK     = 0x2E64;   // green
static const uint16_t C_BAD    = 0xC945;   // red

// The panel hides ~40 px behind the bezel on every side (same as ChessBot),
// so the whole dashboard lives in a centred safe area. Tune SAFE_* if the
// bezel still clips.
static const int16_t SAFE_X = 40, SAFE_Y = 40;
static const int16_t SAFE_W = 320 - 2 * SAFE_X;   // 240
static const int16_t SAFE_H = 170 - 2 * SAFE_Y;   // 90

// compact layout, font 2 (16 px): title, three rows, footer
static const int16_t TITLE_Y  = SAFE_Y;
static const int16_t ROW_Y0   = SAFE_Y + 20;
static const int16_t ROW_STEP = 17;
static const int16_t FOOT_Y   = SAFE_Y + 74;
static const int16_t DOT_X    = SAFE_X + 62;
static const int16_t DETAIL_X = SAFE_X + 76;

static void drawHeader() {
  tft.setTextFont(2);
  tft.setTextColor(C_ACCENT, C_BG);
  tft.setCursor(SAFE_X, TITLE_Y);
  tft.print("PRINT BRIDGE");
  tft.drawFastHLine(SAFE_X, TITLE_Y + 17, SAFE_W, C_PANEL);
}

void displayInit() {
  tft.init();
  tft.setRotation(1);   // flipped landscape (USB on the other side)
  tft.fillScreen(C_BG);
  drawHeader();
}

static const char SPIN[] = { '|', '/', '-', '\\' };

void displayBoot(const char* line) {
  static uint32_t lastDraw = 0;
  if (millis() - lastDraw < 150) return;
  lastDraw = millis();
  tft.setTextFont(2);
  tft.setTextColor(C_TEXT, C_BG);
  tft.setCursor(SAFE_X, ROW_Y0);
  tft.print(line);
  tft.setTextColor(C_ACCENT, C_BG);
  tft.setCursor(SAFE_X + tft.textWidth(line) + 8, ROW_Y0);
  tft.print(SPIN[(millis() / 150) & 3]);
}

// one status row: label, colored state dot, detail text
static void row(uint8_t i, const char* label, bool ok, const char* detail) {
  int16_t y = ROW_Y0 + i * ROW_STEP;
  tft.fillRect(SAFE_X, y, SAFE_W, ROW_STEP, C_BG);
  tft.setTextFont(2);
  tft.setTextColor(C_MUTED, C_BG);
  tft.setCursor(SAFE_X, y);
  tft.print(label);
  tft.fillSmoothCircle(DOT_X, y + 7, 4, ok ? C_OK : C_BAD, C_BG);
  tft.setTextColor(C_TEXT, C_BG);
  tft.setCursor(DETAIL_X, y);
  tft.print(detail);
}

static void footer(const char* left, const char* right, uint16_t rightColor) {
  tft.fillRect(SAFE_X, FOOT_Y - 2, SAFE_W, 18, C_PANEL);
  tft.setTextFont(2);
  tft.setTextColor(C_MUTED, C_PANEL);
  tft.setCursor(SAFE_X + 4, FOOT_Y);
  tft.print(left);
  tft.setTextColor(rightColor, C_PANEL);
  tft.setCursor(SAFE_X + SAFE_W - 4 - tft.textWidth(right), FOOT_Y);
  tft.print(right);
}

void displayTick(const BridgeState& s) {
  static uint32_t lastDraw = 0;
  if (millis() - lastDraw < 250) return;
  lastDraw = millis();

  static bool first = true;
  static BridgeState prev;
  static char prevIp[20] = "";

  if (first) {
    // boot screen leftovers off, header back on
    tft.fillScreen(C_BG);
    drawHeader();
  }

  if (first || s.wifiUp != prev.wifiUp || strcmp(s.wifiUp ? s.ip : "", prevIp) != 0) {
    row(0, "wifi", s.wifiUp, s.wifiUp ? s.ip : "down");
    strlcpy(prevIp, s.wifiUp ? s.ip : "", sizeof(prevIp));
  }
  if (first || s.relayUp != prev.relayUp)
    row(1, "relay", s.relayUp, s.relayUp ? "connected" : "retrying...");
  if (first || s.printerUp != prev.printerUp)
    row(2, "printer", s.printerUp, s.printerUp ? "MPT ready" : "offline");

  // footer: job counters left; activity / uptime right (redraw when text changes)
  static char prevFoot[48] = "";
  char left[24], right[24], both[48];
  snprintf(left, sizeof(left), "jobs %lu  fail %lu",
           (unsigned long)s.jobsDone, (unsigned long)s.jobsFailed);
  if (s.printing) {
    snprintf(right, sizeof(right), "%c print #%lu",
             SPIN[(millis() / 150) & 3], (unsigned long)s.printingJob);
  } else {
    uint32_t up = millis() / 1000;
    snprintf(right, sizeof(right), "up %02lu:%02lu:%02lu",
             (unsigned long)(up / 3600), (unsigned long)(up / 60 % 60),
             (unsigned long)(up % 60));
  }
  snprintf(both, sizeof(both), "%s|%s", left, right);
  if (first || strcmp(both, prevFoot) != 0) {
    footer(left, right, s.printing ? C_ACCENT : C_MUTED);
    strlcpy(prevFoot, both, sizeof(prevFoot));
  }

  prev = s;
  first = false;
}
