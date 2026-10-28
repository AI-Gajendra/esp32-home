# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Two halves in one repo: ESP32 firmware (PlatformIO / Arduino, repo root) and `web/`, the Node uptime dashboard + ingest server it reports to. `README.md` and `web/README.md` are detailed and accurate. Read them before changing fail-safe, relay or monitoring behaviour; they explain *why* each guard exists.

## Commands

Firmware (ESP32 DevKit V1, WROOM-32, 4 MB flash):

```powershell
copy include\config.example.h include\config.h        # first time; config.h is git-ignored and holds all secrets/tunables
pio run -e esp32dev                                    # build only
pio run -e esp32dev -t upload -t monitor               # flash over USB + serial monitor
pio run -e ota -t upload                               # flash over Wi-Fi via tools/ota_upload.py -> esp32-home.local
python tools\ota_upload.py .pio\build\esp32dev\firmware.bin   # same, explicit (MD5-verified)
pio run -t erase                                       # wipe flash incl. NVS
```

Bump `FW_VERSION` in `platformio.ini` `build_flags` before each OTA so the running build is identifiable.

Web (`web/`, Node >= 22.5, ESM, uses built-in `node:sqlite`):

```bash
npm install
npm start          # server/index.js on PORT (3123), needs a .env (see .env.example)
npm run dev        # Vite dev server for client/, proxies /api to 127.0.0.1:3123
npm run build      # client -> web/dist, served statically by the Express server
```

There are no tests or linters in either half (`test/` is the PlatformIO placeholder).

## Firmware architecture

- Every feature is a namespace module: `include/<name>.h` + `src/<name>.cpp` exposing `<name>::begin()` / `<name>::loop()`, wired in `src/main.cpp`. Routes are registered on the single shared `AsyncWebServer` in `setup()`.
- **Init order in `setup()` is deliberate**: `bootguard::begin()` first (decides whether the running OTA image is kept), then `relay::begin()` (pin forced to safe OFF level before the network can touch it), then timekeeping, net, OTA routes, relay routes, app routes, and `monitor::begin()` last.
- `bootguard::loop(net::reachable())` is the definition of "this build is healthy". A freshly OTA'd image is pending until it stays up `OTA_HEALTH_WINDOW_MS` and reports healthy; `OTA_MAX_BOOT_ATTEMPTS` failures roll back to the other slot. When something new becomes essential, add it to that expression.
- `loop()` must never block: use `millis()` deltas. All slow network/TLS work (connectivity probe, ingest POST, Firebase write) lives in the monitor's own FreeRTOS task (16 KB stack, core 1). The monitor's watchdog (forced reconnect after 5 min, reboot after 30 min without a publish) runs in `monitor::loop()` on the main loop, outside the task it supervises.
- Check `ota::isBusy()` before anything heavy; the monitor stands down during an upload.
- `relay::set()` is called from the async web handler, the auto-off timer and the monitor task (remote commands), so it is mutex-guarded. Keep it that way.
- ESPAsyncWebServer matches `^{uri}(/.*)?$` and the first registered route wins, so **register specific routes before general ones** (see `ota::begin()`).
- The OTA module header is `ota_update.h`, never `update.h`: on case-insensitive filesystems it would shadow the core's `Update.h`.
- `partitions_ota_4mb.csv`: dual app slots (`app0`/`app1`, 0x1B0000 each), plus littlefs and coredump. Firmware must fit one slot.
- `include/certs.h` pins root CAs (GTS Root R1 for Google/Firebase, ISRG Root X1 for the ingest server). TLS is verified, never `setInsecure()`.
- `fb_client` supports both legacy DB-secret auth (`FIREBASE_DB_SECRET`, used when non-empty) and email/password ID-token auth.
- Library and platform versions in `platformio.ini` are pinned on purpose (espressif32 6.12.0 = Arduino core 2.0.17, ESPAsyncWebServer 3.12.0).

## Device ↔ server contract

The device is behind NAT and only ever connects outward. Every `MONITOR_PERIOD_MS` (15 s) it publishes the same heartbeat to two sinks:

1. `POST INGEST_URL` (`/api/ingest`, `Authorization: Bearer INGEST_TOKEN`; the token must match `INGEST_TOKEN` in the server's `.env`).
2. Firebase RTDB `devices/<id>/status` (overwritten) and `devices/<id>/events/<start_ts>` (outage backfill on recovery).

The heartbeat JSON is built in `src/monitor.cpp` and consumed in `web/server/index.js` (ingest) and `web/server/firebase.js` (poller). Change both sides together. It carries a three-state connectivity verdict (`online` / `no_internet` / `wifi_down`), `boot_id` and `reset_reason`.

Remote relay control rides on this: `POST /api/relay` (PIN-gated, `web/server/relay.js`) parks a desired state; the ingest reply returns it as `relay_desired`; the device applies it and re-publishes immediately. `toggle` is resolved to absolute on/off server-side when queued, and commands older than `RELAY_COMMAND_TTL_S` are dropped, not applied late.

## Web server architecture

- `server/index.js`: Express 5 app; ingest, the gap detector (every 15 s, opens an outage when *both* sinks are silent for `GAP_SECONDS`), stats/timeline APIs, static hosting of `dist/`. All timing uses the **server's** clock, never the device's.
- `server/store.js`: SQLite (`node:sqlite`); raw heartbeats pruned after `RETAIN_DAYS`, outages/events kept forever.
- `server/prune.js`: hourly Firebase retention sweep. It only deletes `events` children whose key parses as a unix timestamp older than the cutoff; preserve that conservatism.
- `client/`: single-file React 19 dashboard (`App.jsx`). State on the timeline is encoded by segment height first, colour second (colour-vision accessibility). Keep that when changing it.
- Deployed to `~/apps/esphome-uptime` on host `oci-free` (migrated from `oci-free-2`) under pm2 (`ecosystem.config.cjs`) behind nginx; redeploy steps are in `web/README.md`.
- Read-only fallback on Vercel (project `esphome-uptime`, https://esphome-uptime.vercel.app; `vercel deploy --prod` from `web/`): `api/fallback.js` serves the same `/api/*` response shapes rebuilt per request from Firebase alone (no SQLite, ingest or relay control). `vercel.json` rewrites `/api/*` to it, and `.vercelignore` keeps `server/` and `.env` out of the upload. If you change a response shape in `server/index.js`, change the fallback too.
