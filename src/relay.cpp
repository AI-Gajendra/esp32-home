#include "relay.h"

#include <Preferences.h>
#include <esp_system.h>

#include "config.h"

namespace relay {
namespace {

/* Most cheap opto-isolated relay boards pull IN low to energise the coil.
 * If yours is inverted, flip RELAY_ACTIVE_LOW in config.h - nothing else. */
constexpr uint8_t ON_LEVEL  = RELAY_ACTIVE_LOW ? LOW : HIGH;
constexpr uint8_t OFF_LEVEL = RELAY_ACTIVE_LOW ? HIGH : LOW;

Preferences prefs;

bool     s_on      = false;
uint32_t s_since   = 0;   /* millis() when it was switched on */

void drive(bool on) {
    digitalWrite(RELAY_PIN, on ? ON_LEVEL : OFF_LEVEL);
}

bool restoreWanted() {
    if (RELAY_RESTORE_MODE == 0) return false;
    if (RELAY_RESTORE_MODE == 2) return prefs.getBool("on", false);

    /* Mode 1: only after a deliberate software restart. A power cut, a
     * brownout or a crash must not bring the load back on by itself. */
    esp_reset_reason_t r = esp_reset_reason();
    return (r == ESP_RST_SW) && prefs.getBool("on", false);
}

String stateJson() {
    String j = "{";
    j += "\"on\":" + String(s_on ? "true" : "false") + ",";
    j += "\"pin\":" + String(RELAY_PIN) + ",";
    j += "\"active_low\":" + String(RELAY_ACTIVE_LOW ? "true" : "false") + ",";
    j += "\"on_for_ms\":" + String(onForMs()) + ",";
    j += "\"auto_off_in_ms\":" + String(msUntilAutoOff());
    j += "}";
    return j;
}

bool authOk(AsyncWebServerRequest *req) {
#if RELAY_REQUIRE_AUTH
    if (strlen(OTA_USERNAME) == 0) return true;
    if (req->authenticate(OTA_USERNAME, OTA_PASSWORD)) return true;
    req->requestAuthentication();
    return false;
#else
    (void)req;
    return true;
#endif
}

}  // namespace

void begin() {
    /* Order matters: park the output latch at the OFF level first, so the
     * instant the pad becomes an output it is already in a safe state. */
    digitalWrite(RELAY_PIN, OFF_LEVEL);
    pinMode(RELAY_PIN, OUTPUT);
    drive(false);

    prefs.begin("relay", /*readOnly=*/false);

    bool want = restoreWanted();
    Serial.printf("[relay] GPIO %d, active %s, restore mode %d -> starting %s\n",
                  RELAY_PIN, RELAY_ACTIVE_LOW ? "low" : "high",
                  (int)RELAY_RESTORE_MODE, want ? "ON" : "OFF");
    if (want) set(true, "restore");
}

void loop() {
#if RELAY_AUTO_OFF_MS > 0
    if (s_on && millis() - s_since >= (uint32_t)RELAY_AUTO_OFF_MS) {
        Serial.println("[relay] auto-off timer expired");
        set(false, "auto-off");
    }
#endif
}

bool set(bool on, const char *source) {
    if (on == s_on) return s_on;

    s_on = on;
    drive(on);
    if (on) s_since = millis();

    prefs.putBool("on", on);
    Serial.printf("[relay] %s (%s)\n", on ? "ON" : "OFF", source ? source : "?");
    return s_on;
}

bool toggle(const char *source) { return set(!s_on, source); }

bool isOn() { return s_on; }

uint32_t onForMs() { return s_on ? millis() - s_since : 0; }

uint32_t msUntilAutoOff() {
#if RELAY_AUTO_OFF_MS > 0
    if (!s_on) return 0;
    uint32_t elapsed = millis() - s_since;
    return elapsed >= (uint32_t)RELAY_AUTO_OFF_MS ? 0 : (uint32_t)RELAY_AUTO_OFF_MS - elapsed;
#else
    return 0;
#endif
}

void registerRoutes(AsyncWebServer &server) {
    server.on("/api/relay", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(200, "application/json", stateJson());
    });

    server.on("/api/relay", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!authOk(req)) return;

        String want;
        if (req->hasParam("state"))             want = req->getParam("state")->value();
        else if (req->hasParam("state", true))  want = req->getParam("state", true)->value();

        want.toLowerCase();
        if      (want == "on"  || want == "1" || want == "true")  set(true, "http");
        else if (want == "off" || want == "0" || want == "false") set(false, "http");
        else if (want == "toggle")                                toggle("http");
        else {
            req->send(400, "text/plain", "state must be on, off or toggle");
            return;
        }

        req->send(200, "application/json", stateJson());
    });
}

}  // namespace relay
