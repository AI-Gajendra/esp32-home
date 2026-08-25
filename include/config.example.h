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
