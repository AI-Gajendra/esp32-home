#pragma once
#include <Arduino.h>

/* ───────────────────────────────────────────────────────────────────────────
 *  monitor - decides once a minute whether the internet actually works, and
 *  reports that to two independent sinks.
 *
 *  Three states, and the difference between the last two is the whole point:
 *    Online      - associated, and a neutral probe returned 204
 *    NoInternet  - associated to Wi-Fi, router fine, probe failed. The ISP.
 *    WifiDown    - not associated at all. The router, or the device.
 *
 *  An outage can only be reported once it is over, so the device timestamps
 *  the window itself (see timekeeping.h) and backfills it on recovery. The
 *  server independently notices the silence with its own clock, so the two
 *  records cross-check each other.
 *
 *  All network work happens on a dedicated task with a big stack, because TLS
 *  blocks for seconds and must never stall loop() or the async web server.
 *  The supervision that reboots a wedged device deliberately runs on the main
 *  loop instead, so a hang inside the monitor task cannot disable its own
 *  watchdog.
 * ─────────────────────────────────────────────────────────────────────────── */
namespace monitor {

enum class Link : uint8_t { Boot = 0, WifiDown, NoInternet, Online };

void begin();
void loop();                       /* call from loop(): self-heal supervision */

Link        state();
const char *stateName();
bool        online();

uint32_t sequence();
uint32_t secondsSinceGoodPublish();
uint32_t publishCount();
uint32_t publishFailures();
bool     lastServerOk();
bool     lastFirebaseOk();
int32_t  lastProbeMs();
uint8_t  pendingEvents();

}  // namespace monitor
