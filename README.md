# Thermal Receipt Printer — Web Print Station

Print text and images from **evankoza.com** onto an MPT-II Bluetooth thermal
receipt printer, via an ESP32 bridge.

```
Browser (GitHub Pages)          Ubuntu server                ESP32              Printer
┌────────────────────┐  HTTPS  ┌──────────────┐  WebSocket ┌────────────┐  BT  ┌────────┐
│ web/  editor +     │ ──────► │ server/ relay │ ◄───────── │ firmware/  │ ───► │ MPT-II │
│ dither preview     │  POST   │ queue + rate  │  (outbound │ SPP client │ SPP  │ ESC/POS│
└────────────────────┘         │ limit         │   from ESP)└────────────┘      └────────┘
```

The browser does all rendering: it composes text/images on a 384-px-wide
canvas, dithers it client-side (live preview), and sends a packed 1-bit bitmap.
The ESP32 just streams `GS v 0` raster commands to the printer — the MPT-II is
a standard ESC/POS printer (384 dots/line, 203 dpi, ~48 mm printable width).

## 1. Relay (Ubuntu server)

```bash
sudo mkdir -p /opt/thermal-print-relay
sudo cp server/server.js server/package.json /opt/thermal-print-relay/
cd /opt/thermal-print-relay && sudo npm install --omit=dev

# generate a device token and put it in the service file
openssl rand -hex 24
sudo cp server/thermal-print-relay.service /etc/systemd/system/
sudo nano /etc/systemd/system/thermal-print-relay.service   # set DEVICE_TOKEN
sudo systemctl enable --now thermal-print-relay
curl localhost:8377/api/status
```

Expose it as `print.evankoza.com` with your reverse proxy (must also proxy
WebSocket upgrades on `/device`). Caddy example:

```
print.evankoza.com {
    reverse_proxy localhost:8377
}
```

(nginx needs `proxy_set_header Upgrade $http_upgrade; proxy_set_header
Connection "upgrade";` on the `/device` location.)

Built-in protections for public exposure: 3 prints/minute per IP, max 10
queued jobs, max 2400 rows (~30 cm) per print, ESP32 authenticated by token.

## 2. Firmware (ESP32 DevKit / WROOM)

1. Copy `firmware/src/config.example.h` to `firmware/src/config.h` (gitignored)
   and fill in: WiFi credentials, relay host, the same
   `DEVICE_TOKEN` as the service file, and the printer's Bluetooth name.
2. If unsure of the name, set `PRINTER_NAME ""` and flash — it scans and lists
   nearby Bluetooth devices on the serial monitor. Common names: `MPT-II`,
   `MPT-2`, `BlueTooth Printer`. PIN is usually `0000` or `1234`.
3. Build & flash: `pio run -t upload && pio device monitor`
   (PlatformIO; or open in VS Code with the PlatformIO extension).

Turn the printer on, and the serial monitor should show WiFi → relay → printer
all connecting. The bridge auto-reconnects to everything.

## 3. Web editor (GitHub Pages)

`web/index.html` + `web/app.js` are fully static. Set `RELAY_URL` at the top
of `app.js`, then copy both files into your GitHub Pages repo (e.g. under
`/print/`). The status pill in the header shows live printer availability.

Editor features: draggable/resizable text & image elements, fonts/sizes/align,
paper length with auto-fit, and live-tunable print styles — Floyd–Steinberg,
Atkinson, Bayer 4×4/8×8, halftone dots, crosshatch, plain threshold — with
brightness/contrast/threshold/pattern-size/invert controls. What you see in
the preview is pixel-for-pixel what prints.

## Testing locally without hardware

```bash
cd server && DEVICE_TOKEN=test npm start
cd web && python -m http.server 8000   # then set RELAY_URL to http://localhost:8377
```

`localhost:8000` is already in the relay's default CORS allowlist.

`server/fake-esp32.test.js` simulates the ESP32 bridge (connects, reports the
printer online, acks jobs) so you can exercise the whole pipeline with zero
hardware: `DEVICE_TOKEN=test node server.js` + `node fake-esp32.test.js`.
