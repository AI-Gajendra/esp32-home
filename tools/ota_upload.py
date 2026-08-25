#!/usr/bin/env python3
"""Push a firmware.bin to the ESP32's /update endpoint.

Used by `pio run -e ota -t upload`, but it also works standalone:

    python tools/ota_upload.py .pio/build/esp32dev/firmware.bin
    python tools/ota_upload.py --host 192.168.1.42 firmware.bin

Unlike the browser upload it sends an X-Firmware-MD5 header, so the device
verifies the flashed bytes end to end and refuses a corrupted transfer instead
of rebooting into it. Credentials default to whatever is in include/config.h
and can be overridden with --user/--password or OTA_USER/OTA_PASS.
"""

import argparse
import base64
import hashlib
import http.client
import os
import re
import socket
import sys
import time
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
CONFIG_H = os.path.join(HERE, os.pardir, "include", "config.h")
CHUNK = 8192


def from_config(macro, fallback):
    """Read a #define out of include/config.h so the tool needs no setup."""
    try:
        with open(CONFIG_H, "r", encoding="utf-8", errors="replace") as fh:
            m = re.search(r'^\s*#define\s+%s\s+"([^"]*)"' % macro, fh.read(), re.M)
            return m.group(1) if m else fallback
    except OSError:
        return fallback


def human(n):
    return "%.1f KB" % (n / 1024.0) if n < 1048576 else "%.2f MB" % (n / 1048576.0)


def build_body(field, filename, payload):
    boundary = uuid.uuid4().hex
    head = (
        "--%s\r\n"
        'Content-Disposition: form-data; name="%s"; filename="%s"\r\n'
        "Content-Type: application/octet-stream\r\n\r\n" % (boundary, field, filename)
    ).encode()
    tail = ("\r\n--%s--\r\n" % boundary).encode()
    return boundary, head + payload + tail


def main():
    ap = argparse.ArgumentParser(description="OTA-flash an ESP32 over HTTP")
    ap.add_argument("firmware", help="path to firmware.bin")
    ap.add_argument("--host", default=os.environ.get("OTA_HOST", "esp32-home.local"))
    ap.add_argument("--port", type=int, default=int(os.environ.get("OTA_PORT", "80")))
    ap.add_argument("--path", default="/update")
    ap.add_argument("--user", default=os.environ.get("OTA_USER") or from_config("OTA_USERNAME", "admin"))
    ap.add_argument("--password", default=os.environ.get("OTA_PASS") or from_config("OTA_PASSWORD", ""))
    ap.add_argument("--timeout", type=float, default=60.0)
    args = ap.parse_args()

    if not os.path.isfile(args.firmware):
        sys.exit("error: %s does not exist" % args.firmware)

    payload = open(args.firmware, "rb").read()
    if not payload:
        sys.exit("error: firmware file is empty")

    # Catch the classic mistakes locally, before touching the device.
    if payload[0] != 0xE9:
        sys.exit(
            "error: %s does not start with the ESP32 image magic byte 0xE9.\n"
            "       Point this at .pio/build/<env>/firmware.bin - not bootloader.bin,\n"
            "       not partitions.bin, not the .elf." % args.firmware
        )

    digest = hashlib.md5(payload).hexdigest()
    boundary, body = build_body("firmware", os.path.basename(args.firmware), payload)

    print("device   : %s:%d%s" % (args.host, args.port, args.path))
    print("firmware : %s  (%s)" % (args.firmware, human(len(payload))))
    print("md5      : %s" % digest)

    try:
        conn = http.client.HTTPConnection(args.host, args.port, timeout=args.timeout)
        conn.connect()
    except (socket.gaierror, OSError) as e:
        sys.exit(
            "error: cannot reach %s:%d (%s)\n"
            "       If mDNS is not resolving, pass the IP: --host 192.168.x.y"
            % (args.host, args.port, e)
        )

    try:
        conn.putrequest("POST", args.path, skip_accept_encoding=True)
        conn.putheader("Content-Type", "multipart/form-data; boundary=%s" % boundary)
        conn.putheader("Content-Length", str(len(body)))
        conn.putheader("X-Firmware-MD5", digest)
        conn.putheader("Connection", "close")
        if args.user:
            token = base64.b64encode(("%s:%s" % (args.user, args.password)).encode()).decode()
            conn.putheader("Authorization", "Basic " + token)
        conn.endheaders()

        sent, started = 0, time.time()
        while sent < len(body):
            conn.send(body[sent:sent + CHUNK])
            sent += CHUNK
            done = min(sent, len(body))
            pct = done * 100 // len(body)
            bar = "#" * (pct // 4)
            sys.stdout.write("\ruploading [%-25s] %3d%%  %s" % (bar, pct, human(done)))
            sys.stdout.flush()
        elapsed = time.time() - started
        print("\nsent %s in %.1fs (%.0f KB/s)" % (human(len(body)), elapsed,
                                                  len(body) / 1024.0 / max(elapsed, 0.001)))

        resp = conn.getresponse()
        text = resp.read().decode("utf-8", "replace").strip()
    except OSError as e:
        sys.exit("\nerror: transfer failed (%s).\n"
                 "       Nothing was committed - the device is still running the old firmware." % e)
    finally:
        conn.close()

    if resp.status == 200:
        print("device   : %s" % text)
        print("\nOK. The new image must stay alive for its health window or the")
        print("device rolls back to the previous slot by itself.")
        return 0

    if resp.status == 401:
        sys.exit("error: 401 unauthorized - check --user/--password against include/config.h")

    sys.exit("error: device refused the image (HTTP %d): %s" % (resp.status, text))


if __name__ == "__main__":
    sys.exit(main())
