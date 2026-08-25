#include "net.h"

#include <ESPmDNS.h>
#include <WiFi.h>

#include "config.h"

namespace net {
namespace {

bool     s_online     = false;
bool     s_apUp       = false;
bool     s_mdnsUp     = false;
uint32_t s_staStarted = 0;
uint32_t s_lastRetry  = 0;

void startStation() {
    s_staStarted = millis();
    s_lastRetry  = millis();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.printf("[wifi] associating with \"%s\" ...\n", WIFI_SSID);
}

void startFallbackAp() {
    if (s_apUp) return;
    WiFi.mode(WIFI_AP_STA);                       /* keep retrying the station */
    if (WiFi.softAP(AP_FALLBACK_SSID, AP_FALLBACK_PASS)) {
        s_apUp = true;
        Serial.printf("[wifi] FALLBACK AP \"%s\" up at %s - OTA still reachable\n",
                      AP_FALLBACK_SSID, WiFi.softAPIP().toString().c_str());
    } else {
        Serial.println("[wifi] failed to raise fallback AP");
    }
}

void stopFallbackAp() {
    if (!s_apUp) return;
    /* Never yank the AP out from under someone who is connected to it - they
     * might be halfway through an OTA upload. */
    if (WiFi.softAPgetStationNum() > 0) return;
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    s_apUp = false;
    Serial.println("[wifi] station is up and AP is idle - fallback AP shut down");
}

void startMdns() {
    if (s_mdnsUp) return;
    if (MDNS.begin(DEVICE_HOSTNAME)) {
        MDNS.addService("http", "tcp", HTTP_PORT);
        s_mdnsUp = true;
        Serial.printf("[wifi] mDNS: http://%s.local/\n", DEVICE_HOSTNAME);
    }
}

void onEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
    switch (event) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
            s_online = true;
            Serial.printf("[wifi] connected: %s  rssi %d dBm\n",
                          WiFi.localIP().toString().c_str(), WiFi.RSSI());
            startMdns();
            break;

        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
            if (s_online) Serial.printf("[wifi] disconnected (reason %u)\n",
                                        info.wifi_sta_disconnected.reason);
            s_online    = false;
            s_lastRetry = millis();
            break;

        default:
            break;
    }
}

}  // namespace

void begin() {
    WiFi.persistent(false);            /* we own the credentials, not NVS */
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);              /* modem sleep stalls OTA transfers */
    WiFi.setAutoReconnect(true);
    WiFi.setHostname(DEVICE_HOSTNAME);
    WiFi.onEvent(onEvent);
    startStation();
}

void loop() {
    if (s_online) {
        stopFallbackAp();
        return;
    }

    uint32_t now = millis();

    if (!s_apUp && now - s_staStarted > WIFI_CONNECT_TIMEOUT_MS) {
        Serial.println("[wifi] station timed out");
        startFallbackAp();
    }

    /* Keep trying forever - a router that comes back should heal itself. */
    if (now - s_lastRetry > WIFI_RETRY_INTERVAL_MS) {
        s_lastRetry = now;
        WiFi.disconnect();
        startStation();
    }
}

bool isOnline()  { return s_online && WiFi.status() == WL_CONNECTED; }
bool apActive()  { return s_apUp; }
bool reachable() { return isOnline() || apActive(); }

String ip() {
    if (isOnline()) return WiFi.localIP().toString();
    if (s_apUp)     return WiFi.softAPIP().toString();
    return "0.0.0.0";
}

String ssid()     { return isOnline() ? WiFi.SSID() : (s_apUp ? String(AP_FALLBACK_SSID) : String("-")); }
int    rssi()     { return isOnline() ? WiFi.RSSI() : 0; }
String mac()      { return WiFi.macAddress(); }
String hostname() { return String(DEVICE_HOSTNAME); }

String statusText() {
    if (isOnline()) return "station";
    if (s_apUp)     return "fallback-ap";
    return "connecting";
}

}  // namespace net
