# esphome-uptime

The internet-uptime dashboard for the esp32-home monitor.
Live at **https://esphome.sitelala.qzz.io**.

## Why it is built this way

The hard part of an uptime monitor is that the interesting event is the device
*failing to report*. Absence of data is the signal, so something that is always
up has to notice the silence — and it must be possible to tell "my internet
died" apart from "the monitoring plumbing died".

So the ESP32 pushes every heartbeat to **two independent sinks**:

```
                    ┌──────────────► Firebase RTDB ──┐  polled every 20s
ESP32 ── every 60s ─┤                                ├──► this server ──► SQLite
                    └──────────────► POST /api/ingest┘        │
                                                              ├─ gap detector (15s)
                                                              └─ dashboard + API
```

Every arrival is stamped with the **server's** clock, never the device's, so
outage arithmetic never depends on the ESP32 having correct time. An outage is
opened when *both* sinks have been quiet for `GAP_SECONDS` (150s = two and a
half missed beats), and closed the moment anything arrives.

That gives two independent records of every event:

- **the server's**: "we stopped hearing from it at T, for D seconds" — the
  authoritative timing.
- **the device's**: once it recovers, it backfills what it saw during the
  window. If it stayed joined to Wi-Fi the whole time, the outage is tagged
  `no_internet`, which means the router was fine and the ISP was not. That is
  the evidence worth putting in a complaint.

## Layout

```
server/index.js     ingest, gap detection, stats, static hosting
server/store.js     SQLite via node:sqlite - no native module to compile
server/firebase.js  polls the world-readable status node in RTDB
client/             React + Vite dashboard
ecosystem.config.cjs  pm2
```

## Operating it

Deployed at `/home/ubuntu/apps/esphome-uptime` on `oci-free-2`, behind nginx
(`/etc/nginx/sites-available/esphome-uptime`) with a Let's Encrypt certificate,
proxying to `127.0.0.1:3123`. It runs under pm2 and is enabled at boot.

```bash
pm2 logs esphome-uptime          # follow
pm2 restart esphome-uptime       # after a config change
pm2 list
```

To redeploy from a dev machine:

```bash
cd web
tar --exclude=node_modules --exclude=dist --exclude=data -czf - . \
  | ssh oci-free-2 'tar -xzf - -C ~/apps/esphome-uptime'
ssh oci-free-2 'bash -lc "cd ~/apps/esphome-uptime && npm install && npm run build && pm2 restart esphome-uptime"'
```

Configuration is `.env` (never committed) — see `.env.example`. `INGEST_TOKEN`
must match `INGEST_TOKEN` in the firmware's `include/config.h`.

## Firebase retention

Nothing in the Realtime Database expires on its own. `server/prune.js` sweeps
`devices/<id>/events` every hour and deletes children older than
`FIREBASE_RETAIN_DAYS` (default 10). SQLite here is the long-term record;
Firebase only needs a recent window for redundancy.

Keys are listed with `?shallow=true`, so the cost of a sweep grows with the
number of events rather than their size. Only children whose key parses as a
unix timestamp older than the cutoff are deleted - a push id, a stray node, or
anything unparseable is skipped, so the job cannot eat data it does not
understand.

Deleting needs write access, so retention runs only when `FIREBASE_DB_SECRET`
is set; reads are public and need nothing.

## API

| Method | Path | Purpose |
| --- | --- | --- |
| `POST` | `/api/ingest` | heartbeat from the device, `Authorization: Bearer <INGEST_TOKEN>` |
| `GET` | `/api/status` | current state, last seen, per-sink freshness, device info |
| `GET` | `/api/stats` | uptime/downtime for 1h, 24h, 7d, 30d plus 30 daily buckets |
| `GET` | `/api/timeline?range=24h` | outage segments for the connection band |
| `GET` | `/api/outages?range=30d` | the outage log |
| `GET` | `/api/events` | reboots and firmware changes |
| `GET` | `/api/ping` | liveness, used as a probe target |

Everything except `/api/ingest` is public and read-only.

## Dashboard design notes

State on the timeline is encoded by **segment height first** (10 / 30 / 44 px,
rising with severity) with colour as reinforcement, because green-vs-red is the
textbook colour-vision failure: measured CVD ΔE between the good and critical
status hues is 4.1 under deuteranopia, far below the ΔE ≥ 8 bar. Height, the
legend, the hover tooltip and the outage table each carry the same distinction
independently, so no reader depends on hue.

The daily chart plots **minutes of downtime**, not uptime percent — on a 0-100%
axis a good month is thirty identical bars, while downtime minutes make the bad
days the tall ones.
