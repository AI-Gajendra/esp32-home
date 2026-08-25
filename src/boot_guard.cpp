#include "boot_guard.h"

#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>

#include "config.h"

namespace bootguard {
namespace {

constexpr const char *NVS_NS       = "bootguard";
constexpr const char *KEY_PENDING  = "pending";
constexpr const char *KEY_ATTEMPTS = "attempts";

Preferences prefs;

bool     s_pending   = false;   /* running image awaits verification */
bool     s_committed = false;   /* verified during THIS boot          */
uint8_t  s_attempts  = 0;
uint32_t s_bootMs    = 0;

const char *partName(const esp_partition_t *p) { return p ? p->label : "?"; }

/* Point the bootloader at the inactive slot. Does not reboot. */
bool switchToOtherSlot(String &err) {
    const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
    if (!target) {
        err = "no second OTA slot in the partition table";
        return false;
    }

    /* Refuse to boot a slot that has never been written: reading its app
     * descriptor is the cheapest proof that a real image lives there. */
    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(target, &desc) != ESP_OK) {
        err = String("slot ") + partName(target) + " holds no valid image";
        return false;
    }

    esp_err_t e = esp_ota_set_boot_partition(target);
    if (e != ESP_OK) {
        err = String("esp_ota_set_boot_partition: ") + esp_err_to_name(e);
        return false;
    }

    Serial.printf("[boot] next boot -> %s (fw %s, built %s)\n",
                  partName(target), desc.version, desc.date);
    return true;
}

void clearFlags() {
    prefs.putBool(KEY_PENDING, false);
    prefs.putUChar(KEY_ATTEMPTS, 0);
}

}  // namespace

void begin() {
    s_bootMs = millis();
    prefs.begin(NVS_NS, /*readOnly=*/false);

    s_pending  = prefs.getBool(KEY_PENDING, false);
    s_attempts = prefs.getUChar(KEY_ATTEMPTS, 0);

    Serial.printf("[boot] slot=%s reset=%s pending=%s attempts=%u\n",
                  runningSlot(), lastResetReason(),
                  s_pending ? "yes" : "no", s_attempts);

    if (!s_pending) {
        /* Nothing to prove. Make sure the ESP-IDF rollback state machine is
         * satisfied too, in case this image was written by something else. */
        esp_ota_mark_app_valid_cancel_rollback();
        s_committed = true;
        return;
    }

    /* This is a boot of an unverified image: count it before doing anything
     * that could crash, so a crash loop is always visible on the next boot. */
    s_attempts++;
    prefs.putUChar(KEY_ATTEMPTS, s_attempts);
    Serial.printf("[boot] unverified image, attempt %u/%u\n",
                  s_attempts, (unsigned)OTA_MAX_BOOT_ATTEMPTS);

    if (s_attempts > OTA_MAX_BOOT_ATTEMPTS) {
        Serial.println("[boot] too many failed boots -> rolling back");
        String err;
        /* Clear the flags FIRST: if the rollback target is also broken we must
         * not end up ping-ponging between two slots forever. */
        clearFlags();
        if (switchToOtherSlot(err)) {
            Serial.flush();
            delay(200);
            esp_restart();
        }
        Serial.printf("[boot] rollback impossible (%s) - continuing anyway\n",
                      err.c_str());
        s_pending   = false;
        s_committed = true;
        esp_ota_mark_app_valid_cancel_rollback();
    }
}

void loop(bool appHealthy) {
    if (!s_pending || s_committed) return;
    if (!appHealthy) return;
    if (millis() - s_bootMs < OTA_HEALTH_WINDOW_MS) return;

    clearFlags();
    esp_ota_mark_app_valid_cancel_rollback();
    s_pending   = false;
    s_committed = true;
    Serial.printf("[boot] image on %s verified and committed\n", runningSlot());
}

void markPendingVerify() {
    prefs.putBool(KEY_PENDING, true);
    prefs.putUChar(KEY_ATTEMPTS, 0);
    Serial.println("[boot] new image armed for verification on next boot");
}

bool isPending()   { return s_pending && !s_committed; }
bool isCommitted() { return s_committed; }
uint8_t bootAttempts() { return s_attempts; }

uint32_t msUntilCommit() {
    if (!isPending()) return 0;
    uint32_t elapsed = millis() - s_bootMs;
    return elapsed >= OTA_HEALTH_WINDOW_MS ? 0 : OTA_HEALTH_WINDOW_MS - elapsed;
}

bool rollback(String &err) {
    if (switchToOtherSlot(err)) {
        clearFlags();
        return true;
    }
    return false;
}

bool otherSlotUsable() {
    const esp_partition_t *target = esp_ota_get_next_update_partition(nullptr);
    if (!target) return false;
    esp_app_desc_t desc;
    return esp_ota_get_partition_description(target, &desc) == ESP_OK;
}

const char *runningSlot() { return partName(esp_ota_get_running_partition()); }
const char *otherSlot()   { return partName(esp_ota_get_next_update_partition(nullptr)); }

const char *lastResetReason() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "power-on";
        case ESP_RST_EXT:       return "external";
        case ESP_RST_SW:        return "software";
        case ESP_RST_PANIC:     return "panic";
        case ESP_RST_INT_WDT:   return "int-wdt";
        case ESP_RST_TASK_WDT:  return "task-wdt";
        case ESP_RST_WDT:       return "other-wdt";
        case ESP_RST_DEEPSLEEP: return "deep-sleep";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_SDIO:      return "sdio";
        default:                return "unknown";
    }
}

}  // namespace bootguard
