#!/usr/bin/env python3
"""Bluetooth print bridge (Windows) — the desktop is the printer's keeper.

Connects to the thermal-print-relay's /device WebSocket on kohzuhserver (as
if it were the old ESP32) and forwards jobs to the MPT-II through the
Bluetooth SPP virtual COM port Windows creates after pairing. While this PC
is off the relay queues jobs (persisted to disk); the moment this bridge
comes up, the backlog prints.

Liveness is proven, never assumed: Windows happily buffers COM writes to a
printer that has powered off, so a write "succeeding" means nothing. The
only trustworthy signal is a DLE EOT 1 status query answered with a real
byte from the printer. We probe while idle and after every job; a job whose
probe fails is handed back to the relay with {"type":"requeue"} so it prints
when the printer returns, instead of being falsely acked done.

Job protocol (must match server.js):
  - Relay streams a job as binary chunks:
      [0..1] uint16 LE widthBytes (48)
      [2..3] uint16 LE rows in this chunk
      [4..7] uint32 LE uint32 jobId
      [8.. ] widthBytes*rows bitmap bytes
  - Then a text frame {"type":"jobEnd","id":N}.
  - Each chunk prints as a GS v 0 raster block; feed is blank raster rows
    (the MPT-II ignores ESC d); ack {"type":"done"|"error"|"requeue","id":N}.
  - {"type":"status","printer":bool} goes up every few seconds; the relay
    only dispatches jobs while printer=true.
"""

import asyncio
import json
import logging
import threading
import time
from logging.handlers import RotatingFileHandler
from pathlib import Path

import serial
import serial.tools.list_ports
import websockets

RELAY_WS = "ws://100.101.116.82:8377/device"   # kohzuhserver over Tailscale
DEVICE_TOKEN = "dc7526dc476bd7f63be54cdbceb82d74afaf2cb739d26ed6"
PRINTER_BT_MAC = ""       # e.g. "86677ABC30CC" (no colons); empty = first
                          # Bluetooth SPP port with a remote MAC in its hwid

WIDTH_BYTES = 48
FEED_RASTER_ROWS = 40     # ~0.5 cm tail so the print clears the head
BT_WRITE_CHUNK = 256      # pace SPP; printer buffer is small
BT_WRITE_DELAY = 0.03
STATUS_INTERVAL = 5
PROBE_TIMEOUT = 3         # idle probe: an idle printer answers in well under a second
# The MPT-II answers DLE EOT in-line, BEHIND buffered raster data — after a
# job the status byte only arrives once the head finishes printing. So the
# post-job probe must outwait the mechanics (a full 400-row job takes ~10 s;
# generous margin for a cold, slow head).
JOB_PROBE_TIMEOUT = 45

log = logging.getLogger("bridge")
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(message)s",
    handlers=[RotatingFileHandler(Path(__file__).with_name("bridge.log"),
                                  maxBytes=500_000, backupCount=2),
              logging.StreamHandler()],
)

printer = None            # open serial.Serial or None
serial_lock = threading.Lock()  # probes and job writes must never interleave


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


def probe(timeout=PROBE_TIMEOUT):
    """DLE EOT 1: ask the printer for its status byte. A response can only
    come from a live, connected printer — buffers can't counterfeit it.
    Measured quirk: a query landing while the head is busy is DISCARDED, not
    queued — so re-ask every couple of seconds instead of asking once. The
    first answered query after a job doubles as proof the job was digested."""
    try:
        with serial_lock:
            end = time.time() + timeout
            while time.time() < end:
                printer.reset_input_buffer()
                printer.write(b"\x10\x04\x01")
                printer.flush()
                attempt_end = min(time.time() + 2, end)
                while time.time() < attempt_end:
                    if printer.in_waiting:
                        printer.read(printer.in_waiting)
                        return True
                    time.sleep(0.05)
            return False
    except Exception:
        return False


def printer_connect():
    global printer
    port = find_printer_port()
    if not port:
        raise OSError("no Bluetooth SPP COM port found (printer paired?)")
    s = serial.Serial(port, 115200, timeout=5, write_timeout=15)
    s.write(b"\x1b\x40")  # ESC @ init
    s.flush()
    printer = s
    if not probe():       # opening a BT COM port can succeed with nobody home
        printer = None
        s.close()
        raise OSError("port opened but printer did not answer status query")
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
    with serial_lock:
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


async def bridge():
    failed_job = 0    # real error (bad dimensions) — relay should mark it failed
    requeue_job = 0   # printer trouble — relay should put it back in the queue
    printing = False

    async with websockets.connect(f"{RELAY_WS}?token={DEVICE_TOKEN}",
                                  ping_interval=20, ping_timeout=20,
                                  max_size=2**22) as ws:
        log.info("relay connected")

        async def send_status():
            await ws.send(json.dumps({"type": "status",
                                      "printer": printer is not None}))

        async def status_loop():
            while True:
                if printer is None:
                    try:
                        await asyncio.to_thread(printer_connect)
                    except Exception as e:
                        log.info(f"printer connect failed: {e}")
                elif not printing:
                    if not await asyncio.to_thread(probe):
                        log.info("printer stopped answering, marking offline")
                        printer_close()
                await send_status()
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
                    if job_id in (failed_job, requeue_job):
                        continue
                    if width != WIDTH_BYTES or len(msg) - 8 != width * rows:
                        failed_job = job_id
                        continue
                    if printer is None:
                        requeue_job = job_id
                        continue
                    printing = True
                    try:
                        await asyncio.to_thread(print_chunk, rows, bytes(msg[8:]))
                    except Exception as e:
                        log.info(f"print write failed: {e}")
                        printer_close()
                        requeue_job = job_id
                else:
                    m = json.loads(msg)
                    if m.get("type") == "jobEnd":
                        job_id = m["id"]
                        if job_id == failed_job:
                            reply = {"type": "error", "id": job_id,
                                     "message": "bad job data"}
                        elif job_id == requeue_job or printer is None:
                            reply = {"type": "requeue", "id": job_id}
                            log.info(f"job {job_id} handed back (printer offline)")
                        else:
                            try:
                                await asyncio.to_thread(feed_paper)
                            except Exception as e:
                                log.info(f"feed write failed: {e}")
                            # the verdict: the status byte only arrives after
                            # the head finishes, so this outwaits the mechanics
                            if await asyncio.to_thread(probe, JOB_PROBE_TIMEOUT):
                                reply = {"type": "done", "id": job_id}
                                log.info(f"job {job_id} done")
                            else:
                                log.info(f"job {job_id} unconfirmed — printer "
                                         "gone, handing back for reprint")
                                printer_close()
                                reply = {"type": "requeue", "id": job_id}
                        printing = False
                        # forget this id: when the relay re-dispatches the
                        # requeued job it must print, not be skipped again
                        failed_job = requeue_job = 0
                        await send_status()   # printer=false halts dispatch now
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
