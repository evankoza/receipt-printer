#!/usr/bin/env python3
"""Bluetooth print bridge (Windows) — the desktop is the printer's keeper.

Connects to the thermal-print-relay's /device WebSocket on kohzuhserver (as
if it were the old ESP32) and forwards jobs to the MPT-II through the
Bluetooth SPP virtual COM port Windows creates after pairing. While this PC
is off the relay queues jobs (persisted to disk); the moment this bridge
comes up, the backlog prints.

Job protocol (must match server.js):
  - Relay streams a job as binary chunks:
      [0..1] uint16 LE widthBytes (48)
      [2..3] uint16 LE rows in this chunk
      [4..7] uint32 LE jobId
      [8.. ] widthBytes*rows bitmap bytes
  - Then a text frame {"type":"jobEnd","id":N}.
  - Each chunk prints as a GS v 0 raster block; feed is blank raster rows
    (the MPT-II ignores ESC d); ack {"type":"done"|"error","id":N,...}.
  - {"type":"status","printer":bool} goes up every few seconds; the relay
    only dispatches jobs while printer=true.
"""

import asyncio
import json
import logging
import time
from logging.handlers import RotatingFileHandler
from pathlib import Path

import serial
import serial.tools.list_ports
import websockets

RELAY_WS = "ws://100.101.116.82:8377/device"   # kohzuhserver over Tailscale
DEVICE_TOKEN = "dc7526dc476bd7f63be54cdbceb82d74afaf2cb739d26ed6"
PRINTER_BT_MAC = ""       # set after pairing, e.g. "0012F31A5E8C" (no colons);
                          # empty = first Bluetooth SPP port with a remote MAC

WIDTH_BYTES = 48
FEED_RASTER_ROWS = 40     # ~0.5 cm tail so the print clears the head
BT_WRITE_CHUNK = 256      # pace SPP like the ESP32 did; printer buffer is small
BT_WRITE_DELAY = 0.03
STATUS_INTERVAL = 5

log = logging.getLogger("bridge")
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(message)s",
    handlers=[RotatingFileHandler(Path(__file__).with_name("bridge.log"),
                                  maxBytes=500_000, backupCount=2),
              logging.StreamHandler()],
)

printer = None            # open serial.Serial or None


def find_printer_port():
    """The outgoing Bluetooth SPP port carries the remote device's MAC in its
    hardware id (BTHENUM...&<mac>_C...); the incoming one has MAC 000000000000."""
    for p in serial.tools.list_ports.comports():
        hwid = (p.hwid or "").upper()
        if "BTHENUM" not in hwid or "000000000000" in hwid:
            continue
        if PRINTER_BT_MAC and PRINTER_BT_MAC.upper() not in hwid:
            continue
        return p.device
    return None


def printer_connect():
    global printer
    port = find_printer_port()
    if not port:
        raise OSError("no Bluetooth SPP COM port found (printer paired?)")
    s = serial.Serial(port, 115200, timeout=10, write_timeout=15)
    s.write(b"\x1b\x40")  # ESC @ init — also proves the BT link is really up
    s.flush()
    printer = s
    log.info(f"printer connected on {port}")


def printer_close():
    global printer
    if printer:
        try:
            printer.close()
        except Exception:
            pass
        printer = None


def printer_write_all(data: bytes):
    view = memoryview(data)
    while view:
        printer.write(view[:BT_WRITE_CHUNK])
        printer.flush()
        view = view[BT_WRITE_CHUNK:]
        time.sleep(BT_WRITE_DELAY)


def print_chunk(rows: int, bits: bytes):
    hdr = bytes([0x1D, 0x76, 0x30, 0x00,
                 WIDTH_BYTES & 0xFF, WIDTH_BYTES >> 8,
                 rows & 0xFF, rows >> 8])
    printer_write_all(hdr + bits)


def feed_paper():
    hdr = bytes([0x1D, 0x76, 0x30, 0x00,
                 WIDTH_BYTES & 0xFF, WIDTH_BYTES >> 8,
                 FEED_RASTER_ROWS & 0xFF, FEED_RASTER_ROWS >> 8])
    printer_write_all(hdr + bytes(WIDTH_BYTES * FEED_RASTER_ROWS))


def printer_alive():
    """Cheap link probe while idle: a NUL byte is a no-op for ESC/POS."""
    if printer is None:
        return False
    try:
        printer.write(b"\x00")
        printer.flush()
        return True
    except Exception as e:
        log.info(f"printer link lost: {e}")
        printer_close()
        return False


async def bridge():
    failed_job = 0
    printing = False

    async with websockets.connect(f"{RELAY_WS}?token={DEVICE_TOKEN}",
                                  ping_interval=20, ping_timeout=20,
                                  max_size=2**22) as ws:
        log.info("relay connected")

        async def status_loop():
            while True:
                if printer is None:
                    try:
                        await asyncio.to_thread(printer_connect)
                    except Exception as e:
                        log.info(f"printer connect failed: {e}")
                        printer_close()
                elif not printing:
                    await asyncio.to_thread(printer_alive)
                await ws.send(json.dumps({"type": "status",
                                          "printer": printer is not None}))
                await asyncio.sleep(STATUS_INTERVAL)

        status_task = asyncio.create_task(status_loop())
        try:
            async for msg in ws:
                if isinstance(msg, bytes):
                    if len(msg) < 8:
                        continue
                    width = msg[0] | (msg[1] << 8)
                    rows = msg[2] | (msg[3] << 8)
                    job_id = int.from_bytes(msg[4:8], "little")
                    if job_id == failed_job:
                        continue
                    if width != WIDTH_BYTES or len(msg) - 8 != width * rows:
                        failed_job = job_id
                        continue
                    if printer is None:
                        failed_job = job_id
                        continue
                    printing = True
                    try:
                        await asyncio.to_thread(print_chunk, rows, bytes(msg[8:]))
                    except Exception as e:
                        log.info(f"print write failed: {e}")
                        printer_close()
                        failed_job = job_id
                else:
                    m = json.loads(msg)
                    if m.get("type") == "jobEnd":
                        job_id = m["id"]
                        if job_id == failed_job or printer is None:
                            reply = {"type": "error", "id": job_id,
                                     "message": "printer offline or write failed"}
                        else:
                            try:
                                await asyncio.to_thread(feed_paper)
                                reply = {"type": "done", "id": job_id}
                                log.info(f"job {job_id} done")
                            except Exception as e:
                                printer_close()
                                reply = {"type": "error", "id": job_id,
                                         "message": f"feed failed: {e}"}
                        printing = False
                        await ws.send(json.dumps(reply))
        finally:
            status_task.cancel()


async def main():
    while True:
        try:
            await bridge()
        except Exception as e:
            log.info(f"bridge dropped: {e}")
        printer_close()
        await asyncio.sleep(5)


if __name__ == "__main__":
    asyncio.run(main())
