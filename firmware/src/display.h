// Status dashboard on the ideaspark ESP32's integrated 1.9" ST7789 TFT
// (170x320, TFT_eSPI; pins are defined in platformio.ini build_flags).
#pragma once
#include <Arduino.h>

void displayInit();

// Boot phase: called from the blocking WiFi connect loop.
void displayBoot(const char* line);

// Latest bridge state; call every loop, redraws are rate-limited and dirty-checked.
struct BridgeState {
  bool wifiUp;
  const char* ip;        // valid when wifiUp
  bool relayUp;
  bool printerUp;
  uint32_t jobsDone;
  uint32_t jobsFailed;
  bool printing;         // a job's chunks are actively flowing
  uint32_t printingJob;
};
void displayTick(const BridgeState& s);
