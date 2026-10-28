/* Firebase-only stand-in for server/index.js, for running the dashboard on
 * Vercel while the real server is down.
 *
 * Serverless functions have no long-lived process and no durable disk, so
 * there is no SQLite, no gap detector and no direct ingest here. Everything is
 * rebuilt per request from the two things the device keeps writing to the
 * world-readable RTDB regardless:
 *   devices/<id>/status          the latest heartbeat
 *   devices/<id>/events/<start>  outages the device backfilled on recovery
 *
 * The response shapes match server/index.js so the client is unchanged. The
 * differences are honest ones: outage timing is the device's clock rather than
 * the server's, an outage the device could not see (it was off) is invisible,
 * and relay control is reported as not configured because there is no ingest
 * reply to carry a command.
 *
 * vercel.json rewrites /api/<route> here as ?route=<route>.
 */

const CFG = {
  device: process.env.DEVICE_ID || 'esp32-home',
  dbUrl: (process.env.FIREBASE_DB_URL
    || 'https://esp32-home-uptime-default-rtdb.firebaseio.com').replace(/\/$/, ''),
  gapSeconds: Number(process.env.GAP_SECONDS || 150),
  /* The server sweeps events older than this; anything older may be gone, so
   * coverage is only claimed from here on (or from the oldest event left). */
  firebaseRetainDays: Number(process.env.FIREBASE_RETAIN_DAYS || 10),
  heartbeatPeriodS: Number(process.env.HEARTBEAT_PERIOD_S || 60),
};

const RANGES = { '1h': 3600, '24h': 86400, '7d': 604800, '30d': 2592000 };
const now = () => Math.floor(Date.now() / 1000);

async function rtdb(path) {
  const res = await fetch(`${CFG.dbUrl}/devices/${CFG.device}/${path}`, {
    cache: 'no-store',
    signal: AbortSignal.timeout(8000),
  });
  if (!res.ok) throw new Error(`firebase HTTP ${res.status}`);
  return res.json();
}

/** One snapshot of everything the routes need. */
async function load() {
  const ts = now();
  /* Keys are 10-digit unix timestamps, so string order is time order. */
  const since = encodeURIComponent(`"${ts - RANGES['30d']}"`);
  const [status, events] = await Promise.all([
    rtdb('status.json'),
    rtdb(`events.json?orderBy=%22%24key%22&startAt=${since}`),
  ]);

  const hb = status && typeof status === 'object' && Number.isFinite(status.ts) ? status : null;

  const reported = Object.values(events || {})
    .filter((e) => e && e.kind && Number.isFinite(e.start_ts) && Number.isFinite(e.end_ts)
      && e.start_ts >= 1600000000 && e.end_ts >= e.start_ts)
    .map((e) => ({ start: e.start_ts, end: e.end_ts, cause: e.kind, device_confirmed: true }));

  /* Silence now is an outage in progress, the same rule the server applies. */
  const outages = [...reported];
  if (hb && ts - hb.ts > CFG.gapSeconds) {
    outages.push({ start: hb.ts, end: null, cause: 'unreachable', device_confirmed: false });
  }
  outages.sort((a, b) => a.start - b.start);

  const oldest = reported.length ? Math.min(...reported.map((o) => o.start)) : ts;
  const first = hb ? Math.min(oldest, ts - CFG.firebaseRetainDays * 86400) : null;

  return { ts, hb, outages, first };
}

/* ── stats, ported from server/index.js ──────────────────────────────────── */

const overlap = (aFrom, aTo, bFrom, bTo) =>
  Math.max(0, Math.min(aTo, bTo) - Math.max(aFrom, bFrom));

function outageSegments(d, from, to) {
  return d.outages
    .filter((o) => o.start < to && (o.end ?? to) > from)
    .map((o) => {
      const end = o.end ?? to;
      return {
        id: o.start,
        start: o.start,
        end: o.end,
        ongoing: o.end == null,
        duration: end - o.start,
        clipped_start: Math.max(o.start, from),
        clipped_end: Math.min(end, to),
        cause: o.cause,
        device_confirmed: o.device_confirmed,
      };
    });
}

function windowStats(d, from, to) {
  const start = d.first == null ? to : Math.max(from, d.first);
  const covered = Math.max(0, to - start);
  const segs = outageSegments(d, start, to);
  const down = segs.reduce((acc, s) => acc + overlap(s.start, s.end ?? to, start, to), 0);
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
    mtbf_s: segs.length > 1 ? Math.round((covered - down) / segs.length) : null,
    partial: d.first != null && d.first > from,
  };
}

function dailyBuckets(d, days) {
  const to = d.ts;
  const out = [];
  const dayStart = new Date();
  dayStart.setHours(0, 0, 0, 0);
  const todayStart = Math.floor(dayStart.getTime() / 1000);

  for (let i = days - 1; i >= 0; i--) {
    const from = todayStart - i * 86400;
    const until = Math.min(from + 86400, to);
    if (until <= from) continue;
    const known = d.first == null ? 0 : Math.max(0, until - Math.max(from, d.first));
    const segs = outageSegments(d, from, until);
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

function relaySnapshot(d) {
  return {
    state: typeof d.hb?.relay === 'boolean' ? d.hb.relay : null,
    reported_at: d.hb?.ts ?? null,
    desired: null,
    pending: false,
    commanded_at: null,
    commanded_by: null,
    applied_at: null,
    expires_in_s: null,
    eta_s: null,
    /* Nothing can reach the device from here: commands ride on the ingest
     * reply, and ingest goes to the real server. */
    configured: false,
  };
}

/* ── routes ──────────────────────────────────────────────────────────────── */

const routes = {
  status(d) {
    const { ts, hb } = d;
    const open = d.outages.find((o) => o.end == null);
    return {
      device: CFG.device,
      mode: 'firebase-fallback',
      server_ts: ts,
      online: hb ? ts - hb.ts <= CFG.gapSeconds : false,
      last_seen: hb?.ts ?? null,
      seconds_since: hb ? ts - hb.ts : null,
      gap_seconds: CFG.gapSeconds,
      current_outage: open
        ? { start: open.start, duration: ts - open.start, kind: open.cause }
        : null,
      sources: { direct: null, firebase: hb?.ts ?? null },
      device_info: hb
        ? {
            fw: hb.fw, slot: hb.slot, ip: hb.ip, rssi: hb.rssi,
            uptime_s: hb.uptime_s, free_heap: hb.free_heap, boot_id: hb.boot_id,
            relay: hb.relay === true, state: hb.state,
            ssid: hb.ssid ?? null, probe_ms: hb.probe_ms,
            reset_reason: hb.reset_reason ?? null,
            wifi_reconnects: hb.wifi_reconnects ?? null,
            device_ts: hb.ts,
          }
        : null,
      monitoring_since: d.first,
      relay: relaySnapshot(d),
    };
  },

  stats(d) {
    const to = d.ts;
    const ranges = {};
    for (const [name, secs] of Object.entries(RANGES)) ranges[name] = windowStats(d, to - secs, to);
    return { server_ts: to, ranges, daily: dailyBuckets(d, 30) };
  },

  timeline(d, q) {
    const to = d.ts;
    const from = to - (RANGES[q.range] ?? RANGES['24h']);
    return {
      from, to,
      known_from: d.first == null ? to : Math.max(from, d.first),
      segments: outageSegments(d, from, to),
    };
  },

  outages(d, q) {
    const to = d.ts;
    const from = to - (RANGES[q.range] ?? RANGES['30d']);
    return {
      outages: outageSegments(d, from, to)
        .sort((a, b) => b.start - a.start)
        .map(({ id, start, end, ongoing, duration, cause, device_confirmed }) =>
          ({ id, start, end, ongoing, duration, cause, device_confirmed })),
    };
  },

  /* Boot history lives in the server's SQLite. The current boot can still be
   * worked out from the heartbeat: it happened uptime_s before it was sent. */
  events(d) {
    const hb = d.hb;
    if (!hb || !Number.isFinite(hb.uptime_s)) return { events: [] };
    return {
      events: [{
        id: hb.boot_id ?? 0,
        device: CFG.device,
        ts: hb.ts - hb.uptime_s,
        kind: 'boot',
        detail: `boot #${hb.boot_id ?? '?'}, reset: ${hb.reset_reason || '?'}`,
      }],
    };
  },

  relay: (d) => relaySnapshot(d),
};

export default async function handler(req, res) {
  const route = String(req.query.route || '').replace(/\/$/, '');

  if (route === 'ping') return res.status(200).setHeader('content-type', 'text/plain').send('pong');
  if (route === 'health') {
    return res.status(200).json({ ok: true, device: CFG.device, mode: 'firebase-fallback', ts: now() });
  }
  if (route === 'ingest' || (route === 'relay' && req.method === 'POST')) {
    return res.status(503).json({ error: 'read-only fallback: the ingest server is down' });
  }

  const fn = routes[route];
  if (!fn || req.method !== 'GET') return res.status(404).json({ error: 'not found' });

  try {
    const body = fn(await load(), req.query);
    /* Five routes per dashboard refresh, each reading Firebase: let the edge
     * absorb repeats from many viewers. */
    res.setHeader('cache-control', 'public, s-maxage=10, stale-while-revalidate=20');
    return res.status(200).json(body);
  } catch (err) {
    return res.status(502).json({ error: err.message });
  }
}
