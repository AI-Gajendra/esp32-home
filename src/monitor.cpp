#include "monitor.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "boot_guard.h"
#include "certs.h"
#include "config.h"
#include "fb_client.h"
#include "net.h"
#include "ota_update.h"
#include "relay.h"
#include "timekeeping.h"
#include "version.h"

namespace monitor {
namespace {

constexpr uint8_t  MAX_PENDING_EVENTS = 8;
constexpr uint32_t TASK_STACK         = 16384;   /* TLS needs far more than loop() has */
constexpr uint16_t INGEST_TIMEOUT_MS  = 12000;

struct Outage {
    char   kind[16];
    time_t start;
};

struct Event {
    char   kind[16];
    time_t start;
    time_t end;
};

Preferences prefs;

volatile Link     s_state         = Link::Boot;
volatile uint32_t s_seq           = 0;
volatile uint32_t s_publishOk     = 0;
volatile uint32_t s_publishFail   = 0;
volatile uint32_t s_lastGoodMs    = 0;   /* millis() of the last accepted publish */
volatile bool     s_serverOk      = false;
volatile bool     s_firebaseOk    = false;
volatile int32_t  s_probeMs       = -1;
volatile uint8_t  s_pendingCount  = 0;
volatile bool     s_reconnectAsked = false;
volatile bool     s_forceCycle     = false;   /* publish now, do not wait out the minute */

Outage s_outage      = { "", 0 };
bool   s_outageOpen  = false;
Event  s_pending[MAX_PENDING_EVENTS];
uint32_t s_lastCycleMs = 0;

const char *kindFor(Link l) {
    switch (l) {
        case Link::WifiDown:   return "wifi_down";
        case Link::NoInternet: return "no_internet";
        default:               return "online";
    }
}

/* ── outage bookkeeping ──────────────────────────────────────────────────── */

void persistOutage() {
    if (s_outageOpen) {
        prefs.putString("kind", s_outage.kind);
        prefs.putULong("start", (uint32_t)s_outage.start);
    } else {
        prefs.remove("kind");
        prefs.remove("start");
    }
}

void openOutage(const char *kind, time_t start) {
    if (s_outageOpen) return;
    strncpy(s_outage.kind, kind, sizeof(s_outage.kind) - 1);
    s_outage.kind[sizeof(s_outage.kind) - 1] = '\0';
    s_outage.start = start;
    s_outageOpen   = true;
    persistOutage();
    Serial.printf("[mon] outage opened: %s at %s\n", kind, timekeeping::iso(start).c_str());
}

void closeOutage(time_t end) {
    if (!s_outageOpen) return;

    /* Without a usable clock at both ends there is nothing worth reporting -
     * the server's own gap detection still records that we went quiet. */
    if (s_outage.start > 0 && end > s_outage.start && s_pendingCount < MAX_PENDING_EVENTS) {
        Event &e = s_pending[s_pendingCount];
        strncpy(e.kind, s_outage.kind, sizeof(e.kind));
        e.kind[sizeof(e.kind) - 1] = '\0';
        e.start = s_outage.start;
        e.end   = end;
        s_pendingCount++;
        Serial.printf("[mon] outage closed: %s lasted %llds (queued)\n",
                      s_outage.kind, (long long)(end - s_outage.start));
    }
    s_outageOpen = false;
    persistOutage();
}

void transition(Link next) {
    if (next == s_state) return;
    Link prev = s_state;
    s_state = next;

    time_t t = timekeeping::now();
    Serial.printf("[mon] %s -> %s\n",
                  prev == Link::Boot ? "boot" : kindFor(prev), kindFor(next));

    if (next == Link::Online) {
        closeOutage(t);
        return;
    }

    /* A failure that changes character (Wi-Fi dropped during an ISP outage)
     * is recorded as two windows rather than one vague one. */
    if (s_outageOpen && strcmp(s_outage.kind, kindFor(next)) != 0) closeOutage(t);
    openOutage(kindFor(next), t);
}

/* ── probes and publishing ───────────────────────────────────────────────── */

/* Plain HTTP on purpose: this asks "is there real internet", and a captive
 * portal or a dead WAN answers with something other than 204. */
int32_t probeInternet() {
    WiFiClient client;
    HTTPClient http;
    http.setConnectTimeout(MONITOR_PROBE_TIMEOUT_MS);
    http.setTimeout(MONITOR_PROBE_TIMEOUT_MS);
    http.setReuse(false);

    uint32_t t0 = millis();
    if (!http.begin(client, MONITOR_PROBE_URL)) return -1;
    int code = http.GET();
    http.end();

    if (code != MONITOR_PROBE_EXPECT) {
        Serial.printf("[mon] probe returned %d (want %d)\n", code, MONITOR_PROBE_EXPECT);
        return -1;
    }
    return (int32_t)(millis() - t0);
}

String buildPayload() {
    String j;
    j.reserve(768);
    j  = "{\"device\":\"" MONITOR_DEVICE_ID "\"";
    j += ",\"seq\":" + String(s_seq);
    j += ",\"ts\":" + String((uint32_t)timekeeping::now());
    j += ",\"clock\":\"" + String(timekeeping::synced() ? "ntp" : (timekeeping::usable() ? "approx" : "unset")) + "\"";
    j += ",\"state\":\"" + String(kindFor((Link)s_state)) + "\"";
    j += ",\"uptime_s\":" + String(millis() / 1000);
    j += ",\"boot_id\":" + String(bootguard::bootCount());
    j += ",\"reset_reason\":\"" + String(bootguard::lastResetReason()) + "\"";
    j += ",\"rssi\":" + String(net::rssi());
    j += ",\"ip\":\"" + net::ip() + "\"";
    j += ",\"ssid\":\"" + net::ssid() + "\"";
    j += ",\"free_heap\":" + String(ESP.getFreeHeap());
    j += ",\"fw\":\"" FW_VERSION "\"";
    j += ",\"slot\":\"" + String(bootguard::runningSlot()) + "\"";
    j += ",\"relay\":" + String(relay::isOn() ? "true" : "false");
    j += ",\"probe_ms\":" + String(s_probeMs);
    j += ",\"wifi_reconnects\":" + String(net::reconnectCount());
    j += ",\"publish_fails\":" + String(s_publishFail);

    j += ",\"events\":[";
    for (uint8_t i = 0; i < s_pendingCount; i++) {
        if (i) j += ',';
        j += "{\"kind\":\"" + String(s_pending[i].kind) + "\"";
        j += ",\"start_ts\":" + String((uint32_t)s_pending[i].start);
        j += ",\"end_ts\":" + String((uint32_t)s_pending[i].end) + "}";
    }
    j += "]}";
    return j;
}

/* The heartbeat reply is the only channel the server has back to this device -
 * nothing outside the LAN can open a connection to it. So anything the server
 * wants done rides in the answer to a request we made. */
void applyServerReply(const String &body) {
    JsonDocument filter;
    filter["relay_desired"] = true;

    JsonDocument doc;
    if (deserializeJson(doc, body, DeserializationOption::Filter(filter))) return;

    JsonVariant want = doc["relay_desired"];
    if (want.isNull() || !want.is<bool>()) return;   /* null = nothing to do */

    if (want.as<bool>() == relay::isOn()) return;

    Serial.printf("[mon] server asked for relay %s\n", want.as<bool>() ? "ON" : "OFF");
    relay::set(want.as<bool>(), "remote");

    /* Report back straight away rather than waiting out the rest of the
     * minute, so the dashboard stops saying "pending" seconds after the relay
     * actually moved instead of a minute later. */
    s_forceCycle = true;
}

bool postToServer(const String &payload) {
    WiFiClientSecure client;
    client.setCACert(LETSENCRYPT_ROOT_CA);
    client.setTimeout(INGEST_TIMEOUT_MS / 1000);

    HTTPClient http;
    http.setConnectTimeout(INGEST_TIMEOUT_MS);
    http.setTimeout(INGEST_TIMEOUT_MS);
    http.setReuse(false);

    if (!http.begin(client, INGEST_URL)) return false;
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " INGEST_TOKEN);

    int code = http.POST(payload);
    String reply = (code == 200) ? http.getString() : String();
    http.end();

    if (code != 200) {
        Serial.printf("[mon] ingest HTTP %d\n", code);
        return false;
    }
    applyServerReply(reply);
    return true;
}

void publish() {
    s_seq++;
    String payload = buildPayload();
    uint8_t sent = s_pendingCount;

    bool srv = postToServer(payload);

    /* Outages get their own keyed node as well as riding along in `status`.
     * `status` is overwritten every minute, so an outage recorded only there
     * would vanish on the next heartbeat if nothing had read it in between -
     * which is exactly the case Firebase is here to cover. Keyed by start
     * time, so a retry overwrites rather than duplicates. */
    if (fb::ready()) {
        for (uint8_t i = 0; i < sent; i++) {
            String path = String("devices/" MONITOR_DEVICE_ID "/events/") +
                          String((uint32_t)s_pending[i].start);
            String body = "{\"kind\":\"" + String(s_pending[i].kind) + "\"";
            body += ",\"start_ts\":" + String((uint32_t)s_pending[i].start);
            body += ",\"end_ts\":" + String((uint32_t)s_pending[i].end);
            body += ",\"duration_s\":" +
                    String((uint32_t)(s_pending[i].end - s_pending[i].start));
            body += ",\"fw\":\"" FW_VERSION "\"}";
            if (!fb::put(path.c_str(), body)) {
                Serial.printf("[mon] event write failed: %s\n", fb::lastError());
            }
        }
    }

    bool fbo = fb::put("devices/" MONITOR_DEVICE_ID "/status", payload);
    if (!fbo) Serial.printf("[mon] firebase: %s\n", fb::lastError());

    s_serverOk   = srv;
    s_firebaseOk = fbo;

    if (srv || fbo) {
        s_publishOk++;
        s_lastGoodMs     = millis();
        s_reconnectAsked = false;
        /* Only drop the backlog once somebody has actually taken it. Both
         * sinks dedupe on (kind, start), so a double delivery is harmless. */
        if (sent > 0) {
            uint8_t remaining = s_pendingCount - sent;
            for (uint8_t i = 0; i < remaining; i++) s_pending[i] = s_pending[i + sent];
            s_pendingCount = remaining;
        }
        Serial.printf("[mon] #%lu published  server=%s firebase=%s  probe=%ldms heap=%u\n",
                      (unsigned long)s_seq, srv ? "ok" : "FAIL", fbo ? "ok" : "FAIL",
                      (long)s_probeMs, ESP.getFreeHeap());
    } else {
        s_publishFail++;
        Serial.printf("[mon] #%lu publish failed on BOTH sinks\n", (unsigned long)s_seq);
    }
}

void cycle() {
    s_lastCycleMs = millis();

    if (WiFi.status() != WL_CONNECTED) { transition(Link::WifiDown); return; }

    int32_t ms = probeInternet();
    if (ms < 0) { transition(Link::NoInternet); return; }

    s_probeMs = ms;
    transition(Link::Online);
    publish();
}

void task(void *) {
    /* Let the network settle before the first cycle so a boot does not look
     * like an outage. */
    vTaskDelay(pdMS_TO_TICKS(5000));

    for (;;) {
        /* An OTA is streaming megabytes through the same radio and heap -
         * stay out of its way entirely. */
        if (!ota::isBusy()) {
            bool wifiUp = WiFi.status() == WL_CONNECTED;

            /* Catch a dropped association within a second rather than at the
             * next cycle, so outage start times are accurate. */
            if (!wifiUp && s_state != Link::WifiDown) transition(Link::WifiDown);

            if (millis() - s_lastCycleMs >= MONITOR_PERIOD_MS || s_lastCycleMs == 0 ||
                s_forceCycle) {
                s_forceCycle = false;
                cycle();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

}  // namespace

void begin() {
    prefs.begin("monitor", /*readOnly=*/false);
    fb::begin();

    /* Resume an outage that was still open when the device restarted, so a
     * reboot in the middle of one does not erase its start time. */
    String kind = prefs.getString("kind", "");
    uint32_t start = prefs.getULong("start", 0);
    if (kind.length() && start > 0) {
        strncpy(s_outage.kind, kind.c_str(), sizeof(s_outage.kind) - 1);
        s_outage.kind[sizeof(s_outage.kind) - 1] = '\0';
        s_outage.start = (time_t)start;
        s_outageOpen   = true;
        Serial.printf("[mon] resuming outage %s from %s\n",
                      s_outage.kind, timekeeping::iso(s_outage.start).c_str());
    }

    s_lastGoodMs = millis();
    xTaskCreatePinnedToCore(task, "monitor", TASK_STACK, nullptr, 3, nullptr, 1);
    Serial.printf("[mon] started, publishing every %lus to %s and Firebase\n",
                  (unsigned long)(MONITOR_PERIOD_MS / 1000), INGEST_URL);
}

/* Runs on the main task on purpose: if the monitor task ever wedges inside a
 * socket call, the thing that reboots the device must not be wedged with it. */
void loop() {
    if (ota::isBusy()) { s_lastGoodMs = millis(); return; }

    uint32_t silent = millis() - s_lastGoodMs;

    if (silent > MONITOR_REBOOT_AFTER_MS) {
        Serial.printf("[mon] nothing published for %lus - rebooting\n",
                      (unsigned long)(silent / 1000));
        Serial.flush();
        delay(100);
        ESP.restart();
    }

    if (silent > MONITOR_RECONNECT_AFTER_MS && !s_reconnectAsked) {
        s_reconnectAsked = true;
        Serial.printf("[mon] nothing published for %lus - forcing re-association\n",
                      (unsigned long)(silent / 1000));
        net::requestReconnect();
    }
}

Link  state()  { return (Link)s_state; }
bool  online() { return s_state == Link::Online; }

const char *stateName() {
    switch ((Link)s_state) {
        case Link::Online:     return "online";
        case Link::NoInternet: return "no_internet";
        case Link::WifiDown:   return "wifi_down";
        default:               return "boot";
    }
}

uint32_t sequence()                { return s_seq; }
uint32_t publishCount()            { return s_publishOk; }
uint32_t publishFailures()         { return s_publishFail; }
uint32_t secondsSinceGoodPublish() { return (millis() - s_lastGoodMs) / 1000; }
bool     lastServerOk()            { return s_serverOk; }
bool     lastFirebaseOk()          { return s_firebaseOk; }
int32_t  lastProbeMs()             { return s_probeMs; }
uint8_t  pendingEvents()           { return s_pendingCount; }

}  // namespace monitor
