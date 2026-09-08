/* ───────────────────────────────────────────────────────────────────────────
 *  esp32-home  ·  ESP32 DevKit V1
 *
 *  Right now: a hello-world page on the Wi-Fi IP with a relay control button,
 *  plus an OTA update endpoint and upload page from ota_update.h. The structure
 *  is meant to grow - add new modules as <name>.h / <name>.cpp with a
 *  begin()/loop() pair and register their routes in setup().
 * ─────────────────────────────────────────────────────────────────────────── */
#include <Arduino.h>
#include <ESPAsyncWebServer.h>

#include "boot_guard.h"
#include "config.h"
#include "monitor.h"
#include "net.h"
#include "ota_update.h"
#include "relay.h"
#include "timekeeping.h"
#include "version.h"

static AsyncWebServer server(HTTP_PORT);

static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>esp32-home</title>
<style>
  :root{--bg:#11131a;--card:#181b24;--line:#262a36;--fg:#e6e8ee;--dim:#99a0b0;
        --acc:#4c8dff;--ok:#3ddc84;--off:#39404f;--err:#ff5c5c}
  *{box-sizing:border-box}
  body{margin:0;padding:48px 16px;background:var(--bg);color:var(--fg);
       font:15px/1.6 ui-sans-serif,system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
  .wrap{max-width:560px;margin:0 auto}
  h1{font-size:32px;margin:0 0 6px;letter-spacing:-.02em}
  .sub{color:var(--dim);margin-bottom:28px}
  .card{background:var(--card);border:1px solid var(--line);border-radius:12px;
        padding:18px;margin-bottom:16px}
  .card h2{font-size:13px;text-transform:uppercase;letter-spacing:.08em;
           color:var(--dim);margin:0 0 14px;font-weight:600}
  dl{display:grid;grid-template-columns:auto 1fr;gap:6px 16px;margin:0;font-size:13px}
  dt{color:var(--dim)}
  dd{margin:0;text-align:right;font-variant-numeric:tabular-nums}
  .dot{display:inline-block;width:8px;height:8px;border-radius:99px;background:var(--ok);
       margin-right:7px;vertical-align:1px}
  .relay{display:flex;align-items:center;justify-content:space-between;gap:16px}
  .state{font-size:22px;font-weight:700;letter-spacing:.04em}
  .state.on{color:var(--ok)}
  .state.off{color:var(--dim)}
  .sw{position:relative;width:96px;height:48px;border-radius:99px;border:1px solid var(--line);
      background:var(--off);cursor:pointer;padding:0;transition:background .18s;flex:none}
  .sw[data-on="1"]{background:var(--ok)}
  .sw[disabled]{opacity:.5;cursor:progress}
  .sw>span{position:absolute;top:4px;left:4px;width:38px;height:38px;border-radius:99px;
           background:#fff;transition:transform .18s;box-shadow:0 1px 4px rgba(0,0,0,.4)}
  .sw[data-on="1"]>span{transform:translateX(48px)}
  .note{color:var(--dim);font-size:12px;margin-top:12px;min-height:18px}
  .note.err{color:var(--err)}
  a.btn{display:inline-block;margin-top:20px;padding:11px 18px;border-radius:8px;
        background:var(--acc);color:#fff;text-decoration:none;font-weight:600}
</style>
</head>
<body>
<div class="wrap">
  <h1>Hello, world.</h1>
  <div class="sub"><span class="dot"></span>esp32-home is up and serving.</div>

  <div class="card">
    <h2>Relay - GPIO 23</h2>
    <div class="relay">
      <div class="state off" id="rstate">OFF</div>
      <button class="sw" id="sw" data-on="0" aria-label="toggle relay"><span></span></button>
    </div>
    <div class="note" id="rnote"></div>
  </div>

  <div class="card">
    <h2>Device</h2>
    <dl>
      <dt>Firmware</dt><dd id="ver">-</dd>
      <dt>Running slot</dt><dd id="slot">-</dd>
      <dt>Address</dt><dd id="ip">-</dd>
      <dt>Signal</dt><dd id="rssi">-</dd>
      <dt>Uptime</dt><dd id="up">-</dd>
      <dt>Free heap</dt><dd id="heap">-</dd>
    </dl>
    <a class="btn" href="/update">Firmware update</a>
  </div>
</div>
<script>
var $ = function (id) { return document.getElementById(id); };
function dur(s){s=Math.floor(s);var d=Math.floor(s/86400),h=Math.floor(s%86400/3600),
  m=Math.floor(s%3600/60);return (d?d+'d ':'')+(h||d?h+'h ':'')+m+'m '+(s%60)+'s';}

var busy = false;

function paint(on, forMs, autoOffMs) {
  $('sw').setAttribute('data-on', on ? '1' : '0');
  $('rstate').textContent = on ? 'ON' : 'OFF';
  $('rstate').className = 'state ' + (on ? 'on' : 'off');
  var n = '';
  if (on && forMs > 1000) n = 'on for ' + dur(forMs / 1000);
  if (on && autoOffMs > 0) n += (n ? ' - ' : '') + 'auto-off in ' + dur(autoOffMs / 1000);
  $('rnote').textContent = n;
  $('rnote').className = 'note';
}

$('sw').onclick = function () {
  if (busy) return;
  busy = true;
  $('sw').disabled = true;
  fetch('/api/relay?state=toggle', { method: 'POST' }).then(function (r) {
    if (!r.ok) throw new Error('HTTP ' + r.status);
    return r.json();
  }).then(function (d) {
    paint(d.on, d.on_for_ms, d.auto_off_in_ms);
  }).catch(function (e) {
    $('rnote').textContent = 'could not switch the relay (' + e.message + ')';
    $('rnote').className = 'note err';
  }).then(function () {
    busy = false;
    $('sw').disabled = false;
  });
};

function tick(){
  fetch('/health',{cache:'no-store'}).then(function(r){return r.json();}).then(function(d){
    $('ver').textContent  = d.version;
    $('slot').textContent = d.slot + (d.pending ? ' (verifying)' : '');
    $('ip').textContent   = d.ip;
    $('rssi').textContent = d.rssi ? d.rssi + ' dBm' : d.net;
    $('up').textContent   = dur(d.uptime_s);
    $('heap').textContent = (d.free_heap/1024).toFixed(0) + ' KB';
    if (!busy) paint(d.relay, d.relay_on_for_ms, d.relay_auto_off_in_ms);
  }).catch(function(){});
}
tick(); setInterval(tick, 2000);
</script>
</body>
</html>
)HTML";

static String healthJson() {
    String j = "{";
    j += "\"version\":\"" FW_VERSION "\",";
    j += "\"built\":\"" FW_BUILD_DATE "\",";
    j += "\"hostname\":\"" + net::hostname() + "\",";
    j += "\"ip\":\"" + net::ip() + "\",";
    j += "\"net\":\"" + net::statusText() + "\",";
    j += "\"rssi\":" + String(net::rssi()) + ",";
    j += "\"slot\":\"" + String(bootguard::runningSlot()) + "\",";
    j += "\"pending\":" + String(bootguard::isPending() ? "true" : "false") + ",";
    j += "\"relay\":" + String(relay::isOn() ? "true" : "false") + ",";
    j += "\"link\":\"" + String(monitor::stateName()) + "\",";
    j += "\"heartbeat_seq\":" + String(monitor::sequence()) + ",";
    j += "\"since_publish_s\":" + String(monitor::secondsSinceGoodPublish()) + ",";
    j += "\"publish_ok\":" + String(monitor::publishCount()) + ",";
    j += "\"publish_failed\":" + String(monitor::publishFailures()) + ",";
    j += "\"sink_server\":" + String(monitor::lastServerOk() ? "true" : "false") + ",";
    j += "\"sink_firebase\":" + String(monitor::lastFirebaseOk() ? "true" : "false") + ",";
    j += "\"probe_ms\":" + String(monitor::lastProbeMs()) + ",";
    j += "\"pending_events\":" + String(monitor::pendingEvents()) + ",";
    j += "\"boot_id\":" + String(bootguard::bootCount()) + ",";
    j += "\"clock\":\"" + String(timekeeping::synced() ? "ntp" : (timekeeping::usable() ? "approx" : "unset")) + "\",";
    j += "\"relay_on_for_ms\":" + String(relay::onForMs()) + ",";
    j += "\"relay_auto_off_in_ms\":" + String(relay::msUntilAutoOff()) + ",";
    j += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
    j += "\"uptime_s\":" + String(millis() / 1000) + ",";
    j += "\"ota_busy\":" + String(ota::isBusy() ? "true" : "false");
    j += "}";
    return j;
}

void setup() {
    Serial.begin(SERIAL_BAUD);
    delay(300);   /* let the USB-serial bridge settle before the banner */

    Serial.println();
    Serial.println(F("=========================================="));
    Serial.printf("  esp32-home  fw %s\n", FW_VERSION);
    Serial.printf("  built %s\n", FW_BUILD_DATE);
    Serial.printf("  chip %s rev %d, %d MB flash, %u KB heap\n",
                  ESP.getChipModel(), ESP.getChipRevision(),
                  ESP.getFlashChipSize() / (1024 * 1024),
                  (unsigned)(ESP.getHeapSize() / 1024));
    Serial.println(F("=========================================="));

    if (ESP.getFlashChipSize() < 4 * 1024 * 1024) {
        Serial.println(F("[warn] partitions_ota_4mb.csv expects a 4 MB flash - "
                         "OTA will not work on this module"));
    }

    /* First, before anything that can crash: decide whether the image that is
     * running now deserves to keep running. */
    bootguard::begin();

    /* Then the outputs, so the relay reaches a known safe state as early as
     * possible - before the network can ask it to do anything. */
    relay::begin();

    timekeeping::begin();  /* seeds the clock from NVS before anything needs it */
    net::begin();          /* non-blocking - the server comes up either way */
    ota::begin(server);    /* /update and friends */
    relay::registerRoutes(server);

    server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(200, "text/html", (const uint8_t *)INDEX_HTML, strlen_P(INDEX_HTML));
    });

    server.on("/health", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send(200, "application/json", healthJson());
    });

    server.onNotFound([](AsyncWebServerRequest *req) {
        req->send(404, "text/plain", "not found");
    });

    server.begin();
    Serial.printf("[http] listening on port %d\n", HTTP_PORT);

#if MONITOR_ENABLED
    /* Last, so the first heartbeat describes a device that is fully up. */
    monitor::begin();
#endif
}

void loop() {
    net::loop();
    ota::loop();
    relay::loop();
    timekeeping::loop();
#if MONITOR_ENABLED
    monitor::loop();   /* supervises the monitor task from outside it */
#endif

    /* "Healthy" for now means the device is reachable, which is the property
     * that lets you recover it. Extend this as the project grows - sensors
     * initialised, MQTT connected, whatever must work for a build to count. */
    bootguard::loop(net::reachable());

    delay(10);   /* keeps the idle task fed; nothing here blocks */
}
