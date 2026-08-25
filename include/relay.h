#pragma once
#include <Arduino.h>
#include <ESPAsyncWebServer.h>

/* ───────────────────────────────────────────────────────────────────────────
 *  relay - one relay channel on RELAY_PIN (GPIO 23 by default).
 *
 *  Routes (registered by registerRoutes):
 *    GET  /api/relay                 JSON state
 *    POST /api/relay?state=on        also off | toggle
 *
 *  Safety behaviour:
 *    - the pin is driven to the OFF level *before* it becomes an output, so a
 *      reset never flicks the relay through a spurious ON pulse
 *    - after a power loss, a brownout or a panic the relay comes up OFF; the
 *      last state is only restored after a deliberate software reboot (an OTA
 *      or /update/reboot), so an update does not silently switch off whatever
 *      the relay drives. See RELAY_RESTORE_MODE in config.h
 *    - an optional dead-man timer (RELAY_AUTO_OFF_MS) switches the relay off
 *      again after a maximum on-time
 * ─────────────────────────────────────────────────────────────────────────── */
namespace relay {

void begin();
void loop();
void registerRoutes(AsyncWebServer &server);

bool set(bool on, const char *source);   /* returns the new state */
bool toggle(const char *source);
bool isOn();

uint32_t onForMs();          /* how long it has been on, 0 when off */
uint32_t msUntilAutoOff();   /* 0 when off or when the timer is disabled */

}  // namespace relay
