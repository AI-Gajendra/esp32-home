#pragma once
#include <Arduino.h>
#include <time.h>

/* ───────────────────────────────────────────────────────────────────────────
 *  timekeeping - wall-clock time, which the uptime monitor cannot do without.
 *
 *  An outage is reported *after* it ends, so the device has to know when it
 *  started. millis() is not enough across a reboot, so:
 *    - NTP sets the clock whenever the internet is reachable
 *    - the epoch is written to NVS every few minutes
 *    - on a boot with no internet, the clock is seeded from NVS, which is
 *      behind by at most the length of the outage - flagged as approximate so
 *      nothing downstream mistakes it for a real fix
 * ─────────────────────────────────────────────────────────────────────────── */
namespace timekeeping {

void begin();
void loop();

bool     synced();        /* a real NTP sync happened this boot */
bool     usable();        /* synced, or seeded from NVS (approximate)   */
time_t   now();           /* 0 when not usable at all */
uint32_t lastSyncAgeS();  /* seconds since the last NTP sync, 0 if never */
String   iso(time_t t);

}  // namespace timekeeping
