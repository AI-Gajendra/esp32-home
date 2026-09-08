#pragma once
#include <Arduino.h>

/* ───────────────────────────────────────────────────────────────────────────
 *  net - non-blocking Wi-Fi supervision.
 *
 *  setup() never blocks waiting for an association: the web server comes up
 *  immediately either way. If the station cannot join within
 *  WIFI_CONNECT_TIMEOUT_MS, a fallback access point is raised so the OTA page
 *  is still reachable - that is what saves you after flashing a build with the
 *  wrong credentials.
 * ─────────────────────────────────────────────────────────────────────────── */
namespace net {

void begin();
void loop();

bool   isOnline();     /* station associated and has an IP */
bool   apActive();
bool   reachable();    /* either path works -> the OTA page is servable */

String ip();           /* station IP, or the AP IP when in fallback */
String ssid();
int    rssi();
String mac();
String hostname();
String statusText();

/* Ask for a fresh association. Safe to call from another task: the request is
 * a flag, and net::loop() on the main task does the actual work. */
void     requestReconnect();
uint32_t reconnectCount();

}  // namespace net
