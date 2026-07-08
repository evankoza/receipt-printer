// Copy this file to config.h and fill in your values. config.h is gitignored.
#pragma once

// ---- WiFi (2.4 GHz only) ----
#define WIFI_SSID       "YOUR_WIFI_SSID"
#define WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"

// ---- Print relay ----
// Production: your relay's public host, port 443, TLS on.
// Local testing: the relay machine's LAN IP, port 8377, TLS off.
// The token must match DEVICE_TOKEN in the relay's environment.
#define RELAY_HOST      "print.example.com"
#define RELAY_PORT      443
#define RELAY_USE_TLS   true
#define RELAY_PATH      "/device"
#define DEVICE_TOKEN    "GENERATE_WITH_openssl_rand_-hex_24"

// ---- Printer (Bluetooth Classic SPP) ----
// Run once with PRINTER_NAME empty ("") to scan and list nearby BT devices
// on the serial monitor, then fill in the exact name here.
#define PRINTER_NAME    "MPT-11"
#define PRINTER_PIN     "0000"   // try "1234" if pairing fails

// ---- Printing ----
#define PRINTER_WIDTH_BYTES 48   // 384 dots / 8
#define MAX_JOB_HEIGHT      2400 // ~30 cm of paper, matches relay limit
#define STRIPE_ROWS         64   // rows per GS v 0 command
#define BT_CHUNK            256  // bytes per BT write before yielding
#define FEED_LINES_AFTER    6    // legacy ESC d feed (this printer ignores it)
#define FEED_RASTER_ROWS    40   // blank raster rows after each job (~0.5 cm at 203 dpi)
