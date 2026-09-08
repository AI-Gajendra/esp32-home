#include "timekeeping.h"

#include <Preferences.h>
#include <WiFi.h>
#include <sys/time.h>

#include "config.h"

namespace timekeeping {
namespace {

/* Anything before 2025-01-01 means the clock was never set. */
constexpr time_t MIN_VALID_EPOCH = 1735689600;
constexpr uint32_t PERSIST_EVERY_MS = 5UL * 60 * 1000;

Preferences prefs;

bool     s_sntpStarted = false;
bool     s_synced      = false;   /* NTP answered this boot */
bool     s_seeded      = false;   /* clock restored from NVS */
uint32_t s_syncedAtMs  = 0;
uint32_t s_lastPersist = 0;
uint32_t s_lastAttempt = 0;
uint32_t s_seedEpoch   = 0;   /* what NVS said at boot */
uint32_t s_seedAtMs    = 0;

time_t rawNow() {
    time_t t = time(nullptr);
    return t;
}

void persist(time_t t) {
    prefs.putULong("epoch", (uint32_t)t);
    s_lastPersist = millis();
}

}  // namespace

void begin() {
    prefs.begin("clock", /*readOnly=*/false);

    uint32_t saved = prefs.getULong("epoch", 0);
    if (rawNow() < MIN_VALID_EPOCH && saved >= (uint32_t)MIN_VALID_EPOCH) {
        struct timeval tv = { .tv_sec = (time_t)saved, .tv_usec = 0 };
        settimeofday(&tv, nullptr);
        s_seeded    = true;
        s_seedEpoch = saved;
        s_seedAtMs  = millis();
        Serial.printf("[time] seeded from NVS: %s (approximate)\n",
                      iso((time_t)saved).c_str());
    } else if (saved == 0) {
        Serial.println("[time] no saved epoch - waiting for NTP");
    }
}

void loop() {
    /* SNTP can only be started once the interface is up; after that the IDF
     * keeps it in sync on its own, so this runs exactly once per association. */
    if (!s_sntpStarted && WiFi.status() == WL_CONNECTED) {
        configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
        s_sntpStarted = true;
        s_lastAttempt = millis();
        Serial.println("[time] NTP requested");
    }

    /* A dropped association invalidates nothing, but a fresh one should
     * re-arm the request in case the first attempt was made before DHCP. */
    if (s_sntpStarted && !s_synced && WiFi.status() == WL_CONNECTED &&
        millis() - s_lastAttempt > 60000) {
        configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2, NTP_SERVER_3);
        s_lastAttempt = millis();
    }
    if (WiFi.status() != WL_CONNECTED) s_sntpStarted = false;

    time_t t = rawNow();
    if (!s_synced && t >= MIN_VALID_EPOCH && s_sntpStarted) {
        /* A seeded clock also passes the epoch test, so "synced" means the
         * time moved to somewhere the seed could not have taken it: either
         * there was no seed, or SNTP stepped the clock away from where the
         * seed plus elapsed millis() said it should be. */
        bool stepped = true;
        if (s_seeded) {
            time_t expected = (time_t)(s_seedEpoch + (millis() - s_seedAtMs) / 1000);
            stepped = (t > expected + 30) || (t < expected - 30);
        }
        if (stepped) {
            s_synced     = true;
            s_syncedAtMs = millis();
            Serial.printf("[time] synced: %s UTC\n", iso(t).c_str());
            persist(t);
        }
    }

    if (usable() && millis() - s_lastPersist > PERSIST_EVERY_MS) persist(rawNow());
}

bool synced() { return s_synced; }

bool usable() { return rawNow() >= MIN_VALID_EPOCH; }

time_t now() {
    time_t t = rawNow();
    return t >= MIN_VALID_EPOCH ? t : 0;
}

uint32_t lastSyncAgeS() {
    if (!s_synced) return 0;
    return (millis() - s_syncedAtMs) / 1000;
}

String iso(time_t t) {
    if (t < MIN_VALID_EPOCH) return String("unset");
    struct tm tm;
    gmtime_r(&t, &tm);
    char buf[24];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return String(buf);
}

}  // namespace timekeeping
