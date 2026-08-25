#include "ota_update.h"

#include <Update.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include "boot_guard.h"
#include "config.h"
#include "net.h"
#include "update_page.h"
#include "version.h"

namespace ota {
namespace {

/* ── Upload state machine ────────────────────────────────────────────────────
 * Exactly one upload may be in flight. s_owner is the request that was granted
 * the flash; every callback checks it, so a second client cannot interleave
 * bytes into someone else's image. */
enum class Phase : uint8_t { Idle, Receiving, Success, Failed };

Phase                 s_phase    = Phase::Idle;
AsyncWebServerRequest *s_owner   = nullptr;
size_t                s_received = 0;
size_t                s_total    = 0;
uint8_t               s_lastLog  = 0;
String                s_error;
uint32_t              s_rebootAt = 0;   /* 0 = no reboot pending */

constexpr size_t MIN_FREE_HEAP = 24 * 1024;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

bool authOk(AsyncWebServerRequest *req) {
    if (strlen(OTA_USERNAME) == 0) return true;              /* auth disabled */
    return req->authenticate(OTA_USERNAME, OTA_PASSWORD);
}

/* Returns false and answers with 401 when the caller is not authenticated.
 * Only ever call this from a request handler - never from an upload callback,
 * which must not produce a response of its own. */
bool requireAuth(AsyncWebServerRequest *req) {
    if (authOk(req)) return true;
    req->requestAuthentication();
    return false;
}

void scheduleReboot(uint32_t inMs) {
    s_rebootAt = millis() + inMs;
    if (s_rebootAt == 0) s_rebootAt = 1;   /* millis() wrap guard */
}

void fail(const String &why) {
    s_phase = Phase::Failed;
    s_error = why;
    if (Update.isRunning()) Update.abort();
    Serial.printf("[ota] REJECTED: %s\n", why.c_str());
}

const char *updateErrorText() {
    const char *e = Update.errorString();
    return (e && *e) ? e : "unknown flash error";
}

/* The MD5 the client claims the image has. Header first (always available
 * before the body); a form field also works if it precedes the file part. */
String requestedMd5(AsyncWebServerRequest *req) {
    if (req->hasHeader("X-Firmware-MD5")) return req->getHeader("X-Firmware-MD5")->value();
    if (req->hasParam("md5", true))       return req->getParam("md5", true)->value();
    return String();
}

/* Cheap structural validation of the first bytes of an ESP32 image, so a
 * wrong file is refused before a single sector is erased.
 * esp_image_header_t: [0]=magic 0xE9, [12..13]=chip id (LE). */
bool imageHeaderLooksSane(const uint8_t *data, size_t len, String &why) {
    if (len < 1 || data[0] != ESP_IMAGE_HEADER_MAGIC) {
        why = "not an ESP32 firmware image (bad magic byte) - did you pick "
              "firmware.bin and not bootloader.bin or a .elf?";
        return false;
    }
#if defined(CONFIG_IDF_TARGET_ESP32)
    if (len >= 14) {
        uint16_t chipId = (uint16_t)data[12] | ((uint16_t)data[13] << 8);
        if (chipId != ESP_CHIP_ID_ESP32) {
            why = "image is built for a different ESP32 variant (chip id 0x" +
                  String(chipId, HEX) + ")";
            return false;
        }
    }
#endif
    return true;
}

String jsonEscape(const String &in) {
    String out;
    out.reserve(in.length() + 8);
    for (size_t i = 0; i < in.length(); i++) {
        char c = in[i];
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n')        { out += "\\n"; }
        else if ((uint8_t)c < 0x20) { /* drop control chars */ }
        else                       { out += c; }
    }
    return out;
}

/* ── POST /update : the upload itself ─────────────────────────────────────── */

void handleUpload(AsyncWebServerRequest *req, const String &filename, size_t index,
                  uint8_t *data, size_t len, bool final) {
    if (index == 0) {
        /* (1) Authenticate on the first byte. An unauthenticated body is
         *     silently dropped; the request handler is what answers 401. */
        if (!authOk(req)) return;

        /* (2) One at a time. A second uploader is ignored here and gets a 409
         *     from the request handler. */
        if (s_phase == Phase::Receiving && s_owner && s_owner != req) {
            Serial.println("[ota] refused: an upload is already in progress");
            return;
        }

        s_owner    = req;
        s_phase    = Phase::Receiving;
        s_received = 0;
        s_total    = req->contentLength();
        s_lastLog  = 0;
        s_error    = "";

        /* (5) A client that vanishes mid-upload must not leave the flash
         *     half-written and the module wedged as busy. */
        req->onDisconnect([req]() {
            if (s_owner != req) return;
            if (s_phase == Phase::Receiving) {
                Update.abort();
                s_phase = Phase::Failed;
                s_error = "client disconnected mid-upload; nothing was committed";
                Serial.println("[ota] upload aborted - client disconnected");
            }
            s_owner = nullptr;
        });

        Serial.printf("[ota] upload starting: %s (%u bytes incl. multipart overhead)\n",
                      filename.c_str(), (unsigned)s_total);

        if (ESP.getFreeHeap() < MIN_FREE_HEAP) {
            fail("not enough free heap to flash safely (" +
                 String(ESP.getFreeHeap()) + " bytes)");
            return;
        }

        /* (3) Is this even an image for this chip? */
        String why;
        if (!imageHeaderLooksSane(data, len, why)) { fail(why); return; }

        /* (4) Will it fit the slot we are about to write? */
        const esp_partition_t *slot = esp_ota_get_next_update_partition(nullptr);
        if (!slot) { fail("no OTA slot available - check the partition table"); return; }
        if (s_total > slot->size) {
            fail("image (" + String((unsigned)s_total) + " B) is larger than slot " +
                 slot->label + " (" + String((unsigned)slot->size) + " B)");
            return;
        }
        Serial.printf("[ota] target slot %s at 0x%06X, %u bytes\n",
                      slot->label, (unsigned)slot->address, (unsigned)slot->size);

        if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
            fail(String("Update.begin failed: ") + updateErrorText());
            return;
        }

        String md5 = requestedMd5(req);
        if (md5.length() == 32) {
            Update.setMD5(md5.c_str());
            Serial.printf("[ota] will verify against MD5 %s\n", md5.c_str());
        } else if (md5.length() > 0) {
            fail("X-Firmware-MD5 must be exactly 32 hex characters");
            return;
        } else {
            Serial.println("[ota] no MD5 supplied - size check only");
        }
    }

    /* Ignore anything that is not the accepted stream (wrong client, or this
     * stream already failed). */
    if (s_owner != req || s_phase != Phase::Receiving) return;

    if (len && Update.write(data, len) != len) {
        fail(String("flash write failed: ") + updateErrorText());
        return;
    }

    s_received = index + len;
    if (s_total) {
        uint8_t pct = (uint8_t)((uint64_t)s_received * 100 / s_total);
        if (pct >= s_lastLog + 10) {
            s_lastLog = pct - (pct % 10);
            Serial.printf("[ota] %3u%%  (%u / %u bytes)\n", pct,
                          (unsigned)s_received, (unsigned)s_total);
        }
    }

    if (final) {
        /* (6) end(true) checks the declared length and the MD5 if one was set;
         *     only then does it point the bootloader at the new slot. */
        if (!Update.end(true)) {
            fail(String("image rejected on finalise: ") + updateErrorText());
            return;
        }
        s_phase = Phase::Success;
        s_error = "";
        Serial.printf("[ota] flashed %u bytes OK\n", (unsigned)Update.size());

        /* (8) Arm the anti-brick supervision before we hand control over. */
        bootguard::markPendingVerify();

        /* (7) Reboot after the response has had time to reach the browser.
         *     Scheduled here rather than in the request handler so a client
         *     that hangs up at the last moment still gets the new firmware. */
        scheduleReboot(2000);
        Serial.println("[ota] rebooting into the new image in 2s");
    }
}

/* ── POST /update : the reply, after the body has been consumed ──────────── */

void handleUploadResult(AsyncWebServerRequest *req) {
    if (!requireAuth(req)) return;

    if (s_owner != req) {
        req->send(409, "text/plain",
                  "another upload is in progress - try again in a moment");
        return;
    }
    s_owner = nullptr;   /* release the slot either way */

    if (s_phase == Phase::Success) {
        AsyncWebServerResponse *res = req->beginResponse(
            200, "text/plain",
            "OK - flashed and verified. Rebooting into the new firmware.");
        res->addHeader("Connection", "close");
        req->send(res);
        return;
    }

    req->send(400, "text/plain",
              s_error.length() ? s_error : String("upload failed"));
}

/* ── JSON endpoints ──────────────────────────────────────────────────────── */

String infoJson() {
    const esp_partition_t *slot = esp_ota_get_next_update_partition(nullptr);

    String j = "{";
    j += "\"version\":\"" FW_VERSION "\",";
    j += "\"built\":\"" FW_BUILD_DATE "\",";
    j += "\"hostname\":\"" + jsonEscape(net::hostname()) + "\",";
    j += "\"ip\":\"" + net::ip() + "\",";
    j += "\"net\":\"" + net::statusText() + "\",";
    j += "\"ssid\":\"" + jsonEscape(net::ssid()) + "\",";
    j += "\"rssi\":" + String(net::rssi()) + ",";
    j += "\"slot\":\"" + String(bootguard::runningSlot()) + "\",";
    j += "\"other_slot\":\"" + String(bootguard::otherSlot()) + "\",";
    j += "\"other_usable\":" + String(bootguard::otherSlotUsable() ? "true" : "false") + ",";
    j += "\"pending\":" + String(bootguard::isPending() ? "true" : "false") + ",";
    j += "\"commit_in_ms\":" + String(bootguard::msUntilCommit()) + ",";
    j += "\"boot_attempts\":" + String(bootguard::bootAttempts()) + ",";
    j += "\"reset_reason\":\"" + String(bootguard::lastResetReason()) + "\",";
    j += "\"sketch_size\":" + String(ESP.getSketchSize()) + ",";
    j += "\"slot_size\":" + String(slot ? slot->size : 0) + ",";
    j += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
    j += "\"uptime_s\":" + String(millis() / 1000) + ",";
    j += "\"busy\":" + String(isBusy() ? "true" : "false");
    j += "}";
    return j;
}

String statusJson() {
    String j = "{";
    j += "\"busy\":" + String(isBusy() ? "true" : "false") + ",";
    j += "\"received\":" + String((unsigned)s_received) + ",";
    j += "\"total\":" + String((unsigned)s_total) + ",";
    j += "\"percent\":" + String(progressPercent()) + ",";
    j += "\"ok\":" + String(s_phase == Phase::Success ? "true" : "false") + ",";
    j += "\"error\":\"" + jsonEscape(s_error) + "\"";
    j += "}";
    return j;
}

}  // namespace

/* ── Public API ──────────────────────────────────────────────────────────── */

void begin(AsyncWebServer &server) {
    /* Registration order matters. ESPAsyncWebServer matches a route as
     * ^{uri}(/.*)?$ by default, so a handler for "/update" also matches
     * "/update/info" — and the first registered handler wins. Every
     * "/update/..." route therefore has to be registered BEFORE "/update"
     * itself. Keep it that way when you add endpoints. */

    server.on("/update/info", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!requireAuth(req)) return;
        req->send(200, "application/json", infoJson());
    });

    server.on("/update/status", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!requireAuth(req)) return;
        req->send(200, "application/json", statusJson());
    });

    server.on("/update/rollback", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!requireAuth(req)) return;
        if (isBusy()) { req->send(409, "text/plain", "an upload is in progress"); return; }

        String err;
        if (!bootguard::rollback(err)) {
            req->send(409, "text/plain", "cannot roll back: " + err);
            return;
        }
        req->send(200, "text/plain",
                  String("rolling back to ") + bootguard::otherSlot() + " and rebooting");
        scheduleReboot(1500);
    });

    server.on("/update/reboot", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!requireAuth(req)) return;
        if (isBusy()) { req->send(409, "text/plain", "an upload is in progress"); return; }
        req->send(200, "text/plain", "rebooting");
        scheduleReboot(1000);
    });

    /* ...and only now the catch-all-ish "/update" pair. */
    server.on("/update", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (!requireAuth(req)) return;
        req->send(200, "text/html", (const uint8_t *)UPDATE_PAGE_HTML,
                  strlen_P(UPDATE_PAGE_HTML));
    });

    server.on("/update", HTTP_POST, handleUploadResult, handleUpload);

    Serial.println("[ota] endpoints ready at /update");
}

void loop() {
    if (s_rebootAt == 0 || (int32_t)(millis() - s_rebootAt) < 0) return;
    s_rebootAt = 0;
    Serial.println("[ota] restarting now");
    Serial.flush();
    delay(100);
    ESP.restart();
}

bool isBusy() { return s_phase == Phase::Receiving; }

uint8_t progressPercent() {
    if (!s_total) return 0;
    return (uint8_t)((uint64_t)s_received * 100 / s_total);
}

const String &lastError() { return s_error; }

}  // namespace ota
