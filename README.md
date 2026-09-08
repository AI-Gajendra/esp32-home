# esp32-home

Hello-world web page plus a fail-safe over-the-air firmware update endpoint, on
an ESP32 DevKit V1 (Type-C, WROOM-32, 4 MB flash).

This is the seed of a bigger project. Everything is a module with a
`begin()` / `loop()` pair so new features slot in without touching the OTA path.

```
platformio.ini              two envs: esp32dev (USB) and ota (over the network)
partitions_ota_4mb.csv      dual app slots so OTA never overwrites what is running
include/
  config.h                  your credentials and tunables  (git-ignored)
  config.example.h          template - copy to config.h
  version.h                 FW_VERSION, build stamp
  net.h                     non-blocking Wi-Fi with fallback AP
  relay.h                   one relay channel on GPIO 23
  monitor.h                 internet up/down watcher, publishes every 60s
  fb_client.h               Firebase Realtime Database REST client
  timekeeping.h             NTP + an NVS-backed clock that survives outages
  certs.h                   pinned root CAs (generated)
  boot_guard.h              anti-brick: verify-or-roll-back supervision
  ota_update.h              the OTA module: endpoints + upload page
  update_page.h             the /update HTML
src/
  main.cpp                  banner, hello-world page, /health, wiring
  net.cpp  boot_guard.cpp  ota_update.cpp  relay.cpp
  monitor.cpp  fb_client.cpp  timekeeping.cpp
web/
  esphome-uptime            the dashboard + ingest server (see web/README.md)
tools/
  ota_upload.py             CLI flasher with MD5 verification and a progress bar
```

> The module header is `ota_update.h`, not `update.h`: on Windows and macOS the
> filesystem is case-insensitive, so a project header named `update.h` shadows
> the Arduino core's `Update.h` and the OTA library silently disappears.

## First flash (USB, once)

```powershell
copy include\config.example.h include\config.h   # then edit SSID / password / OTA password
pio run -e esp32dev -t upload -t monitor
```

The serial banner prints the IP. Open it:

| URL | What |
| --- | --- |
| `http://<ip>/` | hello world, relay button, live device stats |
| `http://<ip>/update` | firmware upload page (asks for the OTA login) |
| `http://<ip>/health` | JSON, no auth |
| `http://esp32-home.local/` | same thing via mDNS |

## Every flash after that (no cable)

Browser: open `/update`, pick `.pio/build/esp32dev/firmware.bin`, press
**Flash firmware**. Progress bar, then the page waits for the reboot and
reconnects on its own.

CLI (this one also verifies an MD5 end-to-end):

```powershell
pio run -e esp32dev                                          # build
python tools\ota_upload.py .pio\build\esp32dev\firmware.bin  # flash
```

or in one step, `pio run -e ota -t upload`.

Bump `FW_VERSION` in `platformio.ini` before each OTA so the page tells you
which build is actually running.

## The fail-safes

The order things go wrong in, and what catches each one:

| What can go wrong | What stops it |
| --- | --- |
| Someone on the LAN pushes firmware | HTTP auth, checked on the **first byte** of the upload, before any flash write. Browsers use digest so the password never crosses the wire |
| Two uploads at once | One owner at a time; the second gets `409` and is never allowed to interleave bytes |
| Wrong file (`bootloader.bin`, an `.elf`, a photo) | Image magic byte `0xE9` and the chip id are checked from the first chunk; `tools/ota_upload.py` checks the same thing before it even connects |
| Image built for an ESP32-S3/C3 | Chip id in the image header must match |
| Image too big for the slot | Declared length is compared to the standby partition size up front, so it fails in a second instead of at 99% |
| Low memory during the flash | Refused below 24 KB free heap |
| Wi-Fi drops mid-upload | `onDisconnect` aborts the write. The half-written image is in the **standby** slot; the running firmware is untouched |
| Corrupted bytes in transit | `Update.end()` verifies the length and, when the CLI supplies `X-Firmware-MD5`, the MD5 of the flashed bytes. A mismatch means the boot partition is never switched |
| Device reboots before the reply is sent | The reboot is scheduled 2 s after the last chunk, from the upload path, so it happens whether or not the client is still listening |
| **New firmware boots but is broken** | `boot_guard`: a fresh image is "pending". It must stay alive `OTA_HEALTH_WINDOW_MS` (30 s) *and* report itself healthy. Crash or reset 3 times first and the device sets the boot partition back to the previous slot by itself |
| New firmware has the wrong Wi-Fi credentials | After 20 s without an association the device raises its own AP (`esp32-home-setup`), so `/update` is still reachable at `http://192.168.4.1/`. It keeps retrying the real network in the background |
| You just want the old build back | The **Roll back** button on `/update`, or `POST /update/rollback` |
| A panic you did not see | Backtrace lands in the `coredump` partition; `esp32_exception_decoder` in the monitor decodes it |

Normal power cycles never trigger a rollback: only an image flashed over the
air is ever marked pending, so unplugging the board is not counted against it.

### What is deliberately *not* guarded

- **Brownout during the flash.** Power the board from a decent USB supply while
  updating. The brownout detector is left enabled (never disable it) and the
  worst case is a corrupt *standby* slot, which the running firmware survives.
- **Transport encryption.** This is plain HTTP on your LAN. Digest auth keeps
  the password off the wire but the firmware image itself is not encrypted.
- **Image signing.** Secure boot / signed OTA is an ESP-IDF feature that needs
  eFuses burned; worth revisiting only if this ever leaves your network.

## The API

| Method | Path | Auth | Purpose |
| --- | --- | --- | --- |
| `GET` | `/` | no | hello world page |
| `GET` | `/health` | no | JSON: version, slot, ip, heap, uptime |
| `GET` | `/update` | yes | upload page |
| `POST` | `/update` | yes | multipart upload, field `firmware`, optional `X-Firmware-MD5` |
| `GET` | `/update/info` | yes | JSON: version, slots, health, network, free space |
| `GET` | `/update/status` | yes | JSON: live upload progress |
| `POST` | `/update/rollback` | yes | boot the standby slot |
| `POST` | `/update/reboot` | yes | clean restart |
| `GET` | `/api/relay` | no | JSON relay state |
| `POST` | `/api/relay?state=on\|off\|toggle` | no* | switch the relay |

\* set `RELAY_REQUIRE_AUTH` to 1 in `config.h` to put the OTA login in front of
relay control too.

## Internet uptime monitoring

Once a minute the device decides whether the internet actually works and
reports it to two independent places, so the dashboard can tell a real outage
apart from a broken reporting path.

**Three states, and the third is the point:**

| State | Meaning |
| --- | --- |
| `online` | associated, and a neutral probe returned HTTP 204 |
| `no_internet` | associated to your Wi-Fi, router fine, probe failed - **the ISP** |
| `wifi_down` | not associated at all - the router, or the device |

The probe is `connectivitycheck.gstatic.com/generate_204` over plain HTTP: a
captive portal or a dead WAN answers with something other than 204, which is
exactly the case worth catching.

An outage can only be reported after it ends, so the device timestamps the
window itself and backfills it on recovery. That needs a clock across a
reboot, which is what `timekeeping.h` provides: NTP when there is internet,
and an epoch persisted to NVS every five minutes so a device that boots during
an outage still knows roughly what time it is (flagged `approx`, never
confused with a real sync).

Meanwhile the server notices the silence on its own clock. The two records
cross-check each other: the server says *when*, the device says *why*.

**Dashboard:** https://esphome.sitelala.qzz.io

### What lands in Firebase

| Path | Written | Kept |
| --- | --- | --- |
| `devices/esp32-home/status` | every minute, overwritten | latest only |
| `devices/esp32-home/events/<start_ts>` | on recovery from an outage | 10 days |

Outages get their own keyed node as well as riding along in `status`, because
`status` is overwritten every minute - an outage recorded only there would
vanish on the next heartbeat if nothing had read it in between, which is
exactly the case Firebase is there to cover.

Nothing in Firebase expires on its own, so the server sweeps events older than
`FIREBASE_RETAIN_DAYS` (10) every hour. SQLite on the server keeps the
long-term record; Firebase only needs a recent window. The sweeper only
deletes children whose key parses as a unix timestamp older than the cutoff -
anything it cannot identify is left alone.

The device authenticates with a **legacy database secret** (`FIREBASE_DB_SECRET`
in `config.h`). That is simpler than the Firebase Auth path - one string, no
sign-in, nothing to expire - but it bypasses the security rules entirely and
grants full access to the database, and it is recoverable from a firmware
dump. It is acceptable here because the project holds nothing but this
telemetry. Clear that define and fill in `FIREBASE_EMAIL` / `FIREBASE_PASSWORD`
to switch back to scoped ID-token auth; the firmware supports both and picks
whichever is configured.

Database rules stay `read: true` on `devices/`, `write: false` for everyone -
the secret bypasses them, so no other client can write.

**Why it will not quietly go to sleep any more.** The supervision that reboots
a wedged device runs on the main loop, not inside the monitor task - so a hang
inside a socket call cannot disable its own watchdog:

- nothing published for `MONITOR_RECONNECT_AFTER_MS` (5 min) -> force a fresh
  Wi-Fi association
- nothing published for `MONITOR_REBOOT_AFTER_MS` (30 min) -> reboot
- every heartbeat carries `boot_id` and `reset_reason`, so the dashboard shows
  reboots explicitly - from outside, a device that restarted and a device that
  lost its connection look identical without that.

All network work happens on a dedicated 16 KB-stack task, because TLS blocks
for seconds and must never stall `loop()` or the async web server. It also
stands down entirely while an OTA is streaming.

## The relay

One channel on **GPIO 23** (`RELAY_PIN`), with a toggle switch on the home page.

`RELAY_ACTIVE_LOW` defaults to 1 because most opto-isolated relay boards
energise when IN is pulled low. **If ON and OFF come out swapped, that one line
in `include/config.h` is the only thing to change.**

Safety choices worth knowing about, all in `config.h`:

- The pin is driven to the OFF level *before* `pinMode(OUTPUT)`, so a reset
  cannot flick the relay through a spurious ON pulse.
- `RELAY_RESTORE_MODE` is `1`: the last state is restored only after a
  deliberate software restart, which is what an OTA does - so an update does not
  silently switch off whatever the relay drives. After a power cut, a brownout
  or a crash the relay comes up **OFF**, because coming back on unattended is
  the dangerous case. `0` = always off, `2` = always restore.
- `RELAY_AUTO_OFF_MS` is a dead-man timer, off by default. Set it if the relay
  drives something that should never stay on indefinitely.
- The relay does drop out for the ~2 s of an OTA reboot. That is unavoidable
  on a single-MCU design - the pin has no power during reset.

## Growing this

Add `include/thing.h` + `src/thing.cpp` with `thing::begin()` / `thing::loop()`,
call them from `setup()` / `loop()`, and register routes on the shared `server`.

Three rules that keep OTA working as the project grows:

0. **Register specific routes before general ones.** ESPAsyncWebServer matches
   a route as `^{uri}(/.*)?$`, so a handler on `/thing` also answers
   `/thing/status`, and the first one registered wins. `ota::begin()` registers
   every `/update/...` route before `/update` for exactly this reason.

1. **Never block in `loop()`.** Use `millis()` deltas, not `delay()`. A blocked
   loop means a blocked reboot and a dead health check.
2. **Tighten the health check.** `bootguard::loop(net::reachable())` in
   `main.cpp` decides what "this build works" means. When a sensor or a broker
   becomes essential, add it to that expression — that is the condition a bad
   OTA has to fail in order to be rolled back automatically.

Check `ota::isBusy()` before starting anything heavy (long flash writes, big
allocations) so it stays out of the way of an upload in progress.

## Recovering by cable

If everything else fails, USB is always there:

```powershell
pio run -e esp32dev -t upload          # reflash
pio run -t erase                       # wipe flash incl. NVS, then reflash
```
