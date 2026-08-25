#pragma once
#include <Arduino.h>

/* The /update page. Static HTML - every value on it is fetched from
 * /update/info, so there is no template processing on the device. */
static const char UPDATE_PAGE_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Firmware update</title>
<style>
  :root{--bg:#11131a;--card:#181b24;--line:#262a36;--fg:#e6e8ee;--dim:#99a0b0;
        --acc:#4c8dff;--ok:#3ddc84;--warn:#ffb340;--err:#ff5c5c}
  *{box-sizing:border-box}
  body{margin:0;padding:24px 16px;background:var(--bg);color:var(--fg);
       font:15px/1.5 ui-sans-serif,system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
  .wrap{max-width:640px;margin:0 auto}
  h1{font-size:20px;margin:0 0 4px}
  .sub{color:var(--dim);font-size:13px;margin-bottom:20px}
  .card{background:var(--card);border:1px solid var(--line);border-radius:12px;
        padding:18px;margin-bottom:16px}
  .card h2{font-size:13px;text-transform:uppercase;letter-spacing:.08em;
           color:var(--dim);margin:0 0 12px;font-weight:600}
  dl{display:grid;grid-template-columns:auto 1fr;gap:6px 16px;margin:0;font-size:13px}
  dt{color:var(--dim)}
  dd{margin:0;text-align:right;font-variant-numeric:tabular-nums;word-break:break-all}
  input[type=file]{width:100%;padding:10px;background:#0d0f15;border:1px dashed var(--line);
                   border-radius:8px;color:var(--dim);font-size:13px}
  button{font:inherit;font-weight:600;border:0;border-radius:8px;padding:11px 18px;
         cursor:pointer;color:#fff;background:var(--acc)}
  button[disabled]{opacity:.45;cursor:not-allowed}
  button.ghost{background:transparent;border:1px solid var(--line);color:var(--fg)}
  button.danger{background:transparent;border:1px solid var(--err);color:var(--err)}
  .row{display:flex;gap:10px;flex-wrap:wrap;margin-top:14px}
  .bar{height:8px;background:#0d0f15;border-radius:99px;overflow:hidden;margin-top:14px;display:none}
  .bar>i{display:block;height:100%;width:0;background:var(--acc);transition:width .2s}
  .msg{margin-top:12px;font-size:13px;min-height:20px}
  .ok{color:var(--ok)}.err{color:var(--err)}.warn{color:var(--warn)}
  a{color:var(--acc)}
</style>
</head>
<body>
<div class="wrap">
  <h1>Firmware update</h1>
  <div class="sub"><span id="host">device</span> &middot; <a href="/">home</a></div>

  <div class="card">
    <h2>Upload</h2>
    <input type="file" id="file" accept=".bin,application/octet-stream">
    <div class="row">
      <button id="go" disabled>Flash firmware</button>
      <button class="ghost" id="reboot">Reboot</button>
      <button class="danger" id="rollback">Roll back</button>
    </div>
    <div class="bar" id="bar"><i id="fill"></i></div>
    <div class="msg" id="msg"></div>
  </div>

  <div class="card">
    <h2>Device</h2>
    <dl>
      <dt>Firmware</dt><dd id="i-ver">-</dd>
      <dt>Built</dt><dd id="i-built">-</dd>
      <dt>Running slot</dt><dd id="i-slot">-</dd>
      <dt>Health</dt><dd id="i-health">-</dd>
      <dt>Standby slot</dt><dd id="i-other">-</dd>
      <dt>Last reset</dt><dd id="i-reset">-</dd>
      <dt>Uptime</dt><dd id="i-up">-</dd>
      <dt>Free heap</dt><dd id="i-heap">-</dd>
      <dt>Space for image</dt><dd id="i-space">-</dd>
      <dt>Network</dt><dd id="i-net">-</dd>
    </dl>
  </div>
</div>

<script>
var $ = function (id) { return document.getElementById(id); };
var kb = function (n) {
  return n >= 1048576 ? (n / 1048576).toFixed(2) + ' MB' : (n / 1024).toFixed(0) + ' KB';
};
var dur = function (s) {
  s = Math.floor(s);
  var d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60);
  return (d ? d + 'd ' : '') + (h || d ? h + 'h ' : '') + m + 'm ' + (s % 60) + 's';
};
function say(text, cls) {
  var m = $('msg');
  m.textContent = text;
  m.className = 'msg ' + (cls || '');
}

function refresh() {
  return fetch('/update/info', { cache: 'no-store' }).then(function (r) {
    if (!r.ok) return;
    return r.json().then(function (d) {
      $('host').textContent = d.hostname + ' @ ' + d.ip;
      $('i-ver').textContent = d.version;
      $('i-built').textContent = d.built;
      $('i-slot').textContent = d.slot;
      $('i-health').innerHTML = d.pending
        ? '<span class="warn">unverified &middot; ' + Math.ceil(d.commit_in_ms / 1000) + 's left</span>'
        : '<span class="ok">committed</span>';
      $('i-other').innerHTML = d.other_usable
        ? d.other_slot + ' <span class="ok">(rollback ready)</span>'
        : d.other_slot + ' (empty)';
      $('i-reset').textContent = d.reset_reason + (d.boot_attempts ? ' - attempt ' + d.boot_attempts : '');
      $('i-up').textContent = dur(d.uptime_s);
      $('i-heap').textContent = kb(d.free_heap);
      $('i-space').textContent = kb(d.slot_size) + ' free, image ' + kb(d.sketch_size);
      $('i-net').textContent = d.net + ' - ' + d.ssid + (d.rssi ? ' - ' + d.rssi + ' dBm' : '');
    });
  }).catch(function () { /* device is probably rebooting */ });
}

$('file').onchange = function (e) {
  var f = e.target.files[0];
  $('go').disabled = !f;
  if (f) say(f.name + ' - ' + kb(f.size), '');
};

$('go').onclick = function () {
  var f = $('file').files[0];
  if (!f) return;
  if (f.name.slice(-4) !== '.bin' && !confirm('That does not look like a .bin file. Upload anyway?')) return;

  var fd = new FormData();
  fd.append('firmware', f, f.name);
  var xhr = new XMLHttpRequest();
  xhr.open('POST', '/update');
  $('go').disabled = true;
  $('file').disabled = true;
  $('bar').style.display = 'block';

  xhr.upload.onprogress = function (e) {
    if (!e.lengthComputable) return;
    var p = e.loaded / e.total * 100;
    $('fill').style.width = p.toFixed(1) + '%';
    say('Uploading ' + p.toFixed(0) + '% (' + kb(e.loaded) + ' / ' + kb(e.total) + ')');
  };
  xhr.onload = function () {
    if (xhr.status === 200) {
      $('fill').style.width = '100%';
      say('Flashed. Device is rebooting - this page will reconnect.', 'ok');
      setTimeout(waitForReboot, 4000);
    } else {
      say('Rejected (' + xhr.status + '): ' + xhr.responseText, 'err');
      $('go').disabled = false;
      $('file').disabled = false;
    }
  };
  xhr.onerror = function () {
    say('Connection lost during upload. Nothing was committed - the old firmware is still running.', 'err');
    $('go').disabled = false;
    $('file').disabled = false;
  };
  xhr.send(fd);
};

function waitForReboot() {
  var tries = 0;
  var poke = function () {
    fetch('/update/info', { cache: 'no-store' }).then(function (r) {
      if (r.ok) {
        say('Back online. Watch the health field until it says committed.', 'ok');
        $('go').disabled = false;
        $('file').disabled = false;
        refresh();
        return;
      }
      throw new Error('not ready');
    }).catch(function () {
      if (++tries > 40) {
        say('Device did not come back. It should roll back to the previous firmware on its own.', 'err');
        return;
      }
      setTimeout(poke, 1500);
    });
  };
  poke();
}

$('reboot').onclick = function () {
  if (!confirm('Reboot the device?')) return;
  fetch('/update/reboot', { method: 'POST' }).then(function () {
    say('Rebooting...', 'warn');
    setTimeout(waitForReboot, 3000);
  });
};

$('rollback').onclick = function () {
  if (!confirm('Boot the firmware in the standby slot and restart?')) return;
  fetch('/update/rollback', { method: 'POST' }).then(function (r) {
    return r.text().then(function (t) {
      if (r.ok) { say(t, 'warn'); setTimeout(waitForReboot, 3000); }
      else { say(t, 'err'); }
    });
  });
};

refresh();
setInterval(refresh, 3000);
</script>
</body>
</html>
)HTML";
