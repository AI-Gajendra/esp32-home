#pragma once
#include <Arduino.h>
#include <ESPAsyncWebServer.h>

/* ───────────────────────────────────────────────────────────────────────────
 *  ota - over-the-air firmware update over HTTP (see also update_page.h).
 *
 *  ota::begin(server) registers, all behind HTTP digest auth:
 *
 *    GET  /update            upload page (progress bar, device info, rollback)
 *    POST /update            multipart upload, field name "firmware"
 *    GET  /update/info       JSON: version, slot, health, network, free space
 *    GET  /update/status     JSON: live upload progress
 *    POST /update/rollback   boot the other slot (previous firmware)
 *    POST /update/reboot     clean restart
 *
 *  Optional request header  X-Firmware-MD5: <32 hex>  makes the device verify
 *  the flashed image end-to-end before accepting it. tools/ota_upload.py always
 *  sends it.
 *
 *  Fail-safes, in order of when they fire:
 *    1. auth is checked on the very first byte, before any flash write
 *    2. one upload at a time; a second request is refused with 409
 *    3. the image header is validated (magic byte + chip id) up front
 *    4. the declared size must fit the inactive slot
 *    5. a client that disconnects mid-upload aborts the write
 *    6. Update.end() verifies size and, if supplied, the MD5
 *    7. the reboot is deferred until after the HTTP response is delivered
 *    8. the new image must prove itself or boot_guard rolls it back
 * ─────────────────────────────────────────────────────────────────────────── */
namespace ota {

void begin(AsyncWebServer &server);
void loop();                    /* handles the deferred reboot */

bool          isBusy();         /* an upload is streaming - don't do heavy work */
uint8_t       progressPercent();
const String &lastError();

}  // namespace ota
