#pragma once
/* ───────────────────────────────────────────────────────────────────────────
 *  config.example.h  -  copy to  include/config.h  and fill in your values.
 *  config.h is git-ignored so credentials never leave your machine.
 * ─────────────────────────────────────────────────────────────────────────── */

/* ── Wi-Fi (station mode) ─────────────────────────────────────────────────── */
#define WIFI_SSID               "your-ssid"
#define WIFI_PASS               "your-password"

/* Hostname used for DHCP and mDNS -> http://esp32-home.local/ */
#define DEVICE_HOSTNAME         "esp32-home"

/* ── Fallback access point ────────────────────────────────────────────────────
 * If the station never associates (wrong password, router down, you flashed a
 * build with bad credentials) the device raises its own AP so the OTA page
 * stays reachable at http://192.168.4.1/ . Password must be >= 8 characters. */
#define AP_FALLBACK_SSID        "esp32-home-setup"
#define AP_FALLBACK_PASS        "esp32home"

/* ── OTA endpoint credentials ────────────────────────────────────────────────
 * Anyone who can reach the endpoint can replace your firmware. Change these.
 * Set OTA_USERNAME to "" to disable auth (only sane on an isolated lab LAN). */
#define OTA_USERNAME            "admin"
#define OTA_PASSWORD            "change-me"

/* ── Tunables ─────────────────────────────────────────────────────────────── */
#define HTTP_PORT               80
#define SERIAL_BAUD             115200

#define WIFI_CONNECT_TIMEOUT_MS 20000UL   /* before the fallback AP comes up   */
#define WIFI_RETRY_INTERVAL_MS  30000UL   /* re-attempt station association    */

/* Boot-guard: a freshly flashed image must stay alive this long before it is
 * marked good. If it crashes/reboots more times than allowed first, the device
 * rolls back to the previous slot automatically. */
#define OTA_HEALTH_WINDOW_MS    30000UL
#define OTA_MAX_BOOT_ATTEMPTS   3

/* ── Relay channel ────────────────────────────────────────────────────────── */
#define RELAY_PIN               23

/* 1 = the module energises when IN is pulled LOW (most opto-isolated boards).
 * If ON and OFF come out swapped, this is the only line to change. */
#define RELAY_ACTIVE_LOW        1

/* What the relay does at boot:
 *   0 = always start OFF
 *   1 = restore the last state only after a software restart (OTA, /reboot),
 *       start OFF after a power cut, brownout or crash   <- recommended
 *   2 = always restore the last state */
#define RELAY_RESTORE_MODE      1

/* Dead-man timer: switch off automatically after this long. 0 disables it. */
#define RELAY_AUTO_OFF_MS       0

/* 1 = POST /api/relay needs the OTA login too. The home page button then
 * prompts for it, so leave at 0 unless the LAN is shared. */
#define RELAY_REQUIRE_AUTH      0

/* ── Internet uptime monitor ──────────────────────────────────────────────────
 * Every minute the device decides whether the internet works and reports that
 * to two independent places. Silence in both is what the dashboard treats as an
 * outage, so a failure of either sink is distinguishable from a real one. */
#define MONITOR_ENABLED             1
#define MONITOR_DEVICE_ID           "esp32-home"
#define MONITOR_PERIOD_MS           60000UL

/* Neutral internet probe. A captive portal or a dead WAN answers with
 * something other than 204, which is exactly the case we want to catch:
 * Wi-Fi associated, router happy, no actual internet. */
#define MONITOR_PROBE_URL           "http://connectivitycheck.gstatic.com/generate_204"
#define MONITOR_PROBE_EXPECT        204
#define MONITOR_PROBE_TIMEOUT_MS    6000

/* Sink 1: your own server. */
#define INGEST_URL                  "https://esphome.sitelala.qzz.io/api/ingest"
#define INGEST_TOKEN                "change-me"

/* Sink 2: Firebase Realtime Database, authenticated as a dedicated user whose
 * writes the database rules confine to this device's own subtree. */
#define FIREBASE_API_KEY            "change-me"
#define FIREBASE_DB_URL             "https://your-project-default-rtdb.firebaseio.com"
#define FIREBASE_EMAIL              "device@your-project.firebaseapp.com"
#define FIREBASE_PASSWORD           "change-me"

/* Legacy database secret. When non-empty it is used INSTEAD of the Auth user
 * above: one string, no sign-in, no token refresh, nothing to expire.
 *
 * The trade-off is real and worth knowing: a database secret bypasses the
 * security rules completely and grants full read/write over the entire
 * database. It is recoverable from a firmware dump, so it only belongs in a
 * project that holds nothing but this telemetry. Google has deprecated it.
 * Leave it empty to use FIREBASE_EMAIL / FIREBASE_PASSWORD instead. */
#define FIREBASE_DB_SECRET          ""

/* Self-heal. The device is the thing measuring the outage, so it must not be
 * the thing that is broken: if nothing has been published for this long it
 * re-associates, and if that does not help either, it reboots. */
#define MONITOR_RECONNECT_AFTER_MS  (5UL * 60 * 1000)
#define MONITOR_REBOOT_AFTER_MS     (30UL * 60 * 1000)

/* Wall-clock time. Needed to timestamp an outage the device lived through and
 * reports only afterwards. */
#define NTP_SERVER_1                "pool.ntp.org"
#define NTP_SERVER_2                "time.google.com"
#define NTP_SERVER_3                "time.cloudflare.com"
