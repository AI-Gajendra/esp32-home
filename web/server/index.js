/* esphome-uptime - ingest + gap detection + dashboard API.
 *
 * Two independent views of the same device:
 *   1. the ESP32 POSTs a heartbeat here every minute  (source 'direct')
 *   2. the ESP32 also writes it to Firebase RTDB, which we poll (source 'firebase')
 *
 * Silence on BOTH is what counts as an outage, so a failure of either sink is
 * distinguishable from the internet actually being down. Timestamps used for
 * outage maths are always the server's own clock.
 */
import express from 'express';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { existsSync } from 'node:fs';
import { openStore } from './store.js';
import { makeFirebasePoller } from './firebase.js';
import { makePruner } from './prune.js';
import { makeRelayControl } from './relay.js';

const __dirname = dirname(fileURLToPath(import.meta.url));

/* Secrets live in .env next to the app, never in the repo. */
try {
  process.loadEnvFile(join(__dirname, '..', '.env'));
} catch {
  console.warn('[cfg] no .env found - using defaults and process env');
}

const CFG = {
  port: Number(process.env.PORT || 3123),
  device: process.env.DEVICE_ID || 'esp32-home',
  token: process.env.INGEST_TOKEN || '',
  dbUrl: process.env.FIREBASE_DB_URL || '',
  dbFile: process.env.DB_FILE || join(__dirname, '..', 'data', 'uptime.db'),
  /* A heartbeat is due every 60 s. Two and a half missed beats is an outage -
   * long enough that one dropped packet is not an "outage", short enough to
   * catch a real one within ~2 minutes. */
  gapSeconds: Number(process.env.GAP_SECONDS || 150),
  retainDays: Number(process.env.RETAIN_DAYS || 90),
  firebasePollMs: Number(process.env.FIREBASE_POLL_MS || 20000),
  /* Deleting from Firebase needs write access, which the world-readable rules
   * do not grant - so retention only runs when a credential is configured. */
  dbSecret: process.env.FIREBASE_DB_SECRET || '',
  firebaseRetainDays: Number(process.env.FIREBASE_RETAIN_DAYS || 10),
  firebasePruneMs: Number(process.env.FIREBASE_PRUNE_MS || 3600000),
  /* Relay control. The PIN guards this and nothing else - the rest of the
   * site stays public and read-only. */
  relayPin: process.env.RELAY_PIN || '',
  relayCommandTtlS: Number(process.env.RELAY_COMMAND_TTL_S || 600),
  heartbeatPeriodS: Number(process.env.HEARTBEAT_PERIOD_S || 15),
};

const now = () => Math.floor(Date.now() / 1000);
const store = openStore(CFG.dbFile);
const relay = makeRelayControl({
  pin: CFG.relayPin,
  ttlSeconds: CFG.relayCommandTtlS,
});
const app = express();
app.disable('x-powered-by');
app.set('trust proxy', 1);
app.use(express.json({ limit: '64kb' }));

/* ── ingest ──────────────────────────────────────────────────────────────── */

let lastBoot = { id: null, fw: null, slot: null };

function recordHeartbeat(source, hb) {
  const ts = now();
  store.addHeartbeat(CFG.device, ts, source, hb);

  /* A changed boot id (or an uptime that went backwards) means it restarted -
   * worth showing, because "device rebooted" and "internet died" look the same
   * from the outside otherwise. */
  const bootId = Number.isFinite(hb.boot_id) ? hb.boot_id : null;
  if (bootId != null && lastBoot.id != null && bootId !== lastBoot.id) {
    store.addEvent(CFG.device, ts, 'boot',
      `boot #${bootId}, reset: ${hb.reset_reason || '?'}`);
  }
  if (hb.fw && lastBoot.fw && hb.fw !== lastBoot.fw) {
    store.addEvent(CFG.device, ts, 'fw_change', `${lastBoot.fw} -> ${hb.fw}`);
  }
  if (bootId != null) lastBoot = { id: bootId, fw: hb.fw ?? lastBoot.fw, slot: hb.slot ?? null };

  /* Outages the device reported about itself, backfilled once it recovered.
   * These carry the cause the server cannot see: whether Wi-Fi was up. */
  if (Array.isArray(hb.events)) {
    for (const e of hb.events.slice(0, 20)) {
      if (!e || !e.kind || !Number.isFinite(e.start_ts) || !Number.isFinite(e.end_ts)) continue;
      if (e.start_ts < 1600000000 || e.end_ts < e.start_ts) continue;  // needs a sane clock
      store.addDeviceOutage(CFG.device, String(e.kind), e.start_ts, e.end_ts,
        e.note ? String(e.note).slice(0, 200) : null);
    }
  }

  relay.noteHeartbeat(hb.relay === true ? true : hb.relay === false ? false : undefined);

  closeOutageIfOpen(ts);
  return ts;
}

app.post('/api/ingest', (req, res) => {
  if (CFG.token) {
    const auth = req.get('authorization') || '';
    if (auth !== `Bearer ${CFG.token}`) return res.status(401).json({ error: 'unauthorized' });
  }
  const hb = req.body;
  if (!hb || typeof hb !== 'object') return res.status(400).json({ error: 'bad body' });

  const ts = recordHeartbeat('direct', hb);

  /* The only channel we have to the device: it asked us something, so the
   * answer carries whatever we want it to do. null means "no change". */
  res.json({
    ok: true,
    server_ts: ts,
    gap_seconds: CFG.gapSeconds,
    relay_desired: relay.pendingForDevice(),
  });
});

/* Relay control. PIN-gated and rate-limited; everything else here is public. */
app.post('/api/relay', (req, res) => {
  const ip = req.ip || 'unknown';
  const given = req.get('x-relay-pin') || req.body?.pin || '';

  const auth = relay.authorize(ip, given);
  if (!auth.ok) {
    if (auth.retryAfter) res.set('Retry-After', String(auth.retryAfter));
    return res.status(auth.status).json({ error: auth.error, retry_after_s: auth.retryAfter });
  }

  const want = String(req.query.state ?? req.body?.state ?? '').toLowerCase();
  const result = relay.command(want, ip);
  if (!result.ok) return res.status(400).json({ error: result.error });

  const hb = store.lastHeartbeat(CFG.device);
  const eta = hb ? Math.max(0, CFG.heartbeatPeriodS - (now() - hb.server_ts)) : null;
  res.json({ ok: true, relay: relay.snapshot(eta) });
});

app.get('/api/relay', (_req, res) => {
  const hb = store.lastHeartbeat(CFG.device);
  const eta = hb ? Math.max(0, CFG.heartbeatPeriodS - (now() - hb.server_ts)) : null;
  res.json(relay.snapshot(eta));
});

/* Lets the device confirm it has real internet without loading a third party. */
app.get('/api/ping', (_req, res) => res.type('text/plain').send('pong'));

/* ── outage detection ────────────────────────────────────────────────────── */

function lastSeen() {
  const hb = store.lastHeartbeat(CFG.device);
  return hb ? hb.server_ts : null;
}

function closeOutageIfOpen(ts) {
  const open = store.currentOutage(CFG.device);
  if (open) {
    store.endOutage(open.id, ts);
    console.log(`[outage] ended after ${ts - open.start_ts}s`);
  }
}

function detectorTick() {
  const ts = now();
  const seen = lastSeen();
  if (seen == null) return;                       // never heard from it at all
  if (ts - seen <= CFG.gapSeconds) return;        // still reporting

  if (!store.currentOutage(CFG.device)) {
    store.beginOutage(CFG.device, 'unreachable', seen, null);
    console.log(`[outage] started, last seen ${new Date(seen * 1000).toISOString()}`);
  }
}

/* ── stats ───────────────────────────────────────────────────────────────── */

const overlap = (aFrom, aTo, bFrom, bTo) =>
  Math.max(0, Math.min(aTo, bTo) - Math.max(aFrom, bFrom));

/** Server-detected outages clipped to [from,to], each annotated with the cause
 *  the device reported for the same window (if it managed to tell us later). */
function outageSegments(from, to) {
  const server = store.outagesBetween(CFG.device, from, to);
  const reported = store.deviceOutagesBetween(CFG.device, from - 86400, to);

  return server.map((o) => {
    const end = o.end_ts ?? to;
    const cause = reported.find((r) => overlap(o.start_ts, end, r.start_ts, r.end_ts ?? end) > 0);
    return {
      id: o.id,
      start: o.start_ts,
      end: o.end_ts,
      ongoing: o.end_ts == null,
      duration: (o.end_ts ?? to) - o.start_ts,
      clipped_start: Math.max(o.start_ts, from),
      clipped_end: Math.min(end, to),
      /* unreachable = we simply stopped hearing from it.
       * no_internet  = the device was on Wi-Fi the whole time and said so:
       *                the router was fine and the ISP was not. */
      cause: cause ? cause.kind : 'unreachable',
    };
  });
}

function windowStats(from, to) {
  const first = store.firstSeen(CFG.device);
  const start = first == null ? to : Math.max(from, first);
  const covered = Math.max(0, to - start);
  const segs = outageSegments(start, to);

  const down = segs.reduce(
    (acc, s) => acc + overlap(s.start, s.end ?? to, start, to), 0);

  const durations = segs.map((s) => (s.end ?? to) - s.start);
  return {
    from: start,
    to,
    covered_s: covered,
    down_s: down,
    up_s: Math.max(0, covered - down),
    uptime_pct: covered > 0 ? ((covered - down) / covered) * 100 : null,
    outages: segs.length,
    longest_s: durations.length ? Math.max(...durations) : 0,
    /* mean time between failures: uptime spread across the gaps */
    mtbf_s: segs.length > 1 ? Math.round((covered - down) / segs.length) : null,
    partial: first != null && first > from,
  };
}

function dailyBuckets(days) {
  const to = now();
  const first = store.firstSeen(CFG.device);
  const out = [];
  const dayStart = new Date();
  dayStart.setHours(0, 0, 0, 0);
  const todayStart = Math.floor(dayStart.getTime() / 1000);

  for (let i = days - 1; i >= 0; i--) {
    const from = todayStart - i * 86400;
    const until = Math.min(from + 86400, to);
    if (until <= from) continue;
    const known = first == null ? 0 : Math.max(0, until - Math.max(from, first));
    const segs = outageSegments(from, until);
    const down = segs.reduce((a, s) => a + overlap(s.start, s.end ?? until, from, until), 0);
    out.push({
      day: from,
      covered_s: known,
      down_s: known > 0 ? down : 0,
      uptime_pct: known > 0 ? ((known - down) / known) * 100 : null,
      outages: known > 0 ? segs.length : 0,
    });
  }
  return out;
}

/* ── API ─────────────────────────────────────────────────────────────────── */

const RANGES = { '1h': 3600, '24h': 86400, '7d': 604800, '30d': 2592000, '90d': 7776000 };

app.get('/api/status', (_req, res) => {
  const ts = now();
  const hb = store.lastHeartbeat(CFG.device);
  const open = store.currentOutage(CFG.device);
  const sources = store.lastBySource(CFG.device);
  const raw = hb?.raw ? safeParse(hb.raw) : null;

  res.json({
    device: CFG.device,
    server_ts: ts,
    online: hb ? ts - hb.server_ts <= CFG.gapSeconds : false,
    last_seen: hb?.server_ts ?? null,
    seconds_since: hb ? ts - hb.server_ts : null,
    gap_seconds: CFG.gapSeconds,
    current_outage: open
      ? { start: open.start_ts, duration: ts - open.start_ts, kind: open.kind }
      : null,
    sources: {
      direct: sources.direct ?? null,
      firebase: sources.firebase ?? null,
    },
    device_info: hb
      ? {
          fw: hb.fw, slot: hb.slot, ip: hb.ip, rssi: hb.rssi,
          uptime_s: hb.uptime_s, free_heap: hb.free_heap, boot_id: hb.boot_id,
          relay: hb.relay === 1, state: hb.state,
          ssid: raw?.ssid ?? null, probe_ms: hb.probe_ms,
          reset_reason: raw?.reset_reason ?? null,
          wifi_reconnects: raw?.wifi_reconnects ?? null,
          device_ts: hb.device_ts ?? null,
        }
      : null,
    monitoring_since: store.firstSeen(CFG.device),
    relay: relay.snapshot(hb ? Math.max(0, CFG.heartbeatPeriodS - (ts - hb.server_ts)) : null),
  });
});

app.get('/api/stats', (_req, res) => {
  const to = now();
  const out = {};
  for (const [name, secs] of Object.entries(RANGES)) out[name] = windowStats(to - secs, to);
  /* 90 days so the longest range has a bar per day; the client shows the
   * last 30 unless 90d is selected. */
  res.json({ server_ts: to, ranges: out, daily: dailyBuckets(90) });
});

app.get('/api/timeline', (req, res) => {
  const to = now();
  const secs = RANGES[req.query.range] ?? RANGES['24h'];
  const from = to - secs;
  const first = store.firstSeen(CFG.device);
  res.json({
    from, to,
    known_from: first == null ? to : Math.max(from, first),
    segments: outageSegments(from, to),
  });
});

app.get('/api/outages', (req, res) => {
  const to = now();
  const secs = RANGES[req.query.range] ?? RANGES['30d'];
  const server = store.outagesBetween(CFG.device, to - secs, to).reverse();
  const device = store.deviceOutagesBetween(CFG.device, to - secs, to);

  res.json({
    outages: server.map((o) => {
      const end = o.end_ts ?? to;
      const cause = device.find((d) => overlap(o.start_ts, end, d.start_ts, d.end_ts ?? end) > 0);
      return {
        id: o.id,
        start: o.start_ts,
        end: o.end_ts,
        ongoing: o.end_ts == null,
        duration: end - o.start_ts,
        cause: cause ? cause.kind : 'unreachable',
        device_confirmed: Boolean(cause),
      };
    }),
  });
});

app.get('/api/events', (_req, res) => {
  res.json({ events: store.recentEvents(CFG.device, now() - RANGES['30d']) });
});

app.get('/api/health', (_req, res) => {
  res.json({
    ok: true,
    device: CFG.device,
    firebase: Boolean(CFG.dbUrl),
    firebase_retention_days: CFG.dbSecret ? CFG.firebaseRetainDays : null,
    ts: now(),
  });
});

function safeParse(s) {
  try { return JSON.parse(s); } catch { return null; }
}

/* ── static dashboard ────────────────────────────────────────────────────── */

const dist = join(__dirname, '..', 'dist');
if (existsSync(dist)) {
  app.use(express.static(dist, { maxAge: '1h', index: 'index.html' }));
  app.get(/^\/(?!api\/).*/, (_req, res) => res.sendFile(join(dist, 'index.html')));
} else {
  app.get('/', (_req, res) =>
    res.status(503).type('text/plain').send('dashboard not built yet - run: npm run build'));
}

/* ── start ───────────────────────────────────────────────────────────────── */

setInterval(detectorTick, 15000);
setInterval(() => store.prune(now() - CFG.retainDays * 86400), 6 * 3600 * 1000);

if (CFG.dbUrl) {
  makeFirebasePoller({
    url: CFG.dbUrl,
    device: CFG.device,
    intervalMs: CFG.firebasePollMs,
    onHeartbeat: (hb) => recordHeartbeat('firebase', hb),
  }).start();
  console.log(`[firebase] polling ${CFG.dbUrl} every ${CFG.firebasePollMs}ms`);
} else {
  console.log('[firebase] FIREBASE_DB_URL not set - direct ingest only');
}

const pruner = makePruner({
  dbUrl: CFG.dbUrl,
  secret: CFG.dbSecret,
  device: CFG.device,
  retainDays: CFG.firebaseRetainDays,
  intervalMs: CFG.firebasePruneMs,
});
if (CFG.dbUrl) pruner.start();

app.listen(CFG.port, '127.0.0.1', () => {
  console.log(`[http] esphome-uptime listening on 127.0.0.1:${CFG.port}`);
  console.log(`[cfg]  device=${CFG.device} gap=${CFG.gapSeconds}s db=${CFG.dbFile}`);
});
