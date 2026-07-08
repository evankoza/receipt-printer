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

// landscape layout: 320 wide, 170 tall
static const int16_t ROW_X = 12, ROW_W = 296;
static const int16_t ROW_Y[3] = { 44, 78, 112 };
static const int16_t ROW_H = 30;
static const int16_t FOOT_Y = 148;

void displayInit() {
  tft.init();
  tft.setRotation(3);
  tft.fillScreen(C_BG);
  tft.setTextFont(4);
  tft.setTextColor(C_ACCENT, C_BG);
  tft.setCursor(ROW_X, 8);
  tft.print("PRINT BRIDGE");
  tft.drawFastHLine(0, 36, 320, C_PANEL);
}

static const char SPIN[] = { '|', '/', '-', '\\' };

void displayBoot(const char* line) {
  static uint32_t lastDraw = 0;
  if (millis() - lastDraw < 150) return;
  lastDraw = millis();
  tft.setTextFont(4);
  tft.setTextColor(C_TEXT, C_BG);
  tft.setCursor(ROW_X, ROW_Y[0]);
  tft.print(line);
  tft.setTextColor(C_ACCENT, C_BG);
  tft.setCursor(ROW_X + tft.textWidth(line) + 12, ROW_Y[0]);
  tft.print(SPIN[(millis() / 150) & 3]);
}

// one status row: label, colored state dot, detail text
static void row(uint8_t i, const char* label, bool ok, const char* detail) {
  int16_t y = ROW_Y[i];
  tft.fillRect(ROW_X, y, ROW_W, ROW_H, C_BG);
  tft.setTextFont(4);
  tft.setTextColor(C_MUTED, C_BG);
  tft.setCursor(ROW_X, y);
  tft.print(label);
  tft.fillSmoothCircle(118, y + 11, 7, ok ? C_OK : C_BAD, C_BG);
  tft.setTextColor(C_TEXT, C_BG);
  tft.setCursor(138, y);
  tft.print(detail);
}

static void footer(const char* left, const char* right, uint16_t rightColor) {
  tft.fillRect(0, FOOT_Y - 4, 320, 170 - (FOOT_Y - 4), C_PANEL);
  tft.setTextFont(2);
  tft.setTextColor(C_MUTED, C_PANEL);
  tft.setCursor(ROW_X, FOOT_Y);
  tft.print(left);
  tft.setTextColor(rightColor, C_PANEL);
  tft.setCursor(320 - ROW_X - tft.textWidth(right), FOOT_Y);
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
    tft.setTextFont(4);
    tft.setTextColor(C_ACCENT, C_BG);
    tft.setCursor(ROW_X, 8);
    tft.print("PRINT BRIDGE");
    tft.drawFastHLine(0, 36, 320, C_PANEL);
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
  snprintf(left, sizeof(left), "jobs %lu   failed %lu",
           (unsigned long)s.jobsDone, (unsigned long)s.jobsFailed);
  if (s.printing) {
    snprintf(right, sizeof(right), "%c printing #%lu",
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
