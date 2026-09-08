#pragma once
#include <Arduino.h>

/* ───────────────────────────────────────────────────────────────────────────
 *  boot_guard - anti-brick supervision for OTA firmware.
 *
 *  A freshly flashed image is "pending verification". It must survive
 *  OTA_HEALTH_WINDOW_MS of uptime while the application reports itself healthy;
 *  only then is it committed. If it panics, watchdogs, or is reset before that
 *  happens OTA_MAX_BOOT_ATTEMPTS times in a row, the device switches the boot
 *  partition back to the previous slot and reboots into the last good build.
 *
 *  Images flashed over USB are never marked pending, so ordinary power cycles
 *  can never trigger a rollback.
 * ─────────────────────────────────────────────────────────────────────────── */
namespace bootguard {

/* Call FIRST in setup(), before Wi-Fi or the web server. May reboot. */
void begin();

/* Call every loop(). `appHealthy` is the application's own verdict - pass
 * something meaningful (e.g. "network is up"), not a literal true. */
void loop(bool appHealthy);

void markPendingVerify();          /* called by the OTA handler after a flash  */

bool        isPending();           /* running image not yet committed          */
bool        isCommitted();
uint8_t     bootAttempts();        /* failed boots counted so far              */
uint32_t    bootCount();           /* total boots ever - a reboot and an outage
                                    * look identical from outside without it  */
uint32_t    msUntilCommit();       /* 0 once committed                         */

bool        rollback(String &err); /* manual: boot the other slot, no reboot   */
bool        otherSlotUsable();

const char *runningSlot();         /* "app0" / "app1"                          */
const char *otherSlot();
const char *lastResetReason();

}  // namespace bootguard
