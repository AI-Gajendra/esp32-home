/* SQLite store. Uses node:sqlite (built into Node 22.5+/24) so there is no
 * native module to compile on the ARM box. Booleans are stored as 0/1 because
 * node:sqlite only binds null / number / bigint / string / Uint8Array. */
import { DatabaseSync } from 'node:sqlite';
import { mkdirSync } from 'node:fs';
import { dirname } from 'node:path';

export function openStore(file) {
  mkdirSync(dirname(file), { recursive: true });
  const db = new DatabaseSync(file);

  db.exec(`
    PRAGMA journal_mode = WAL;
    PRAGMA synchronous  = NORMAL;

    CREATE TABLE IF NOT EXISTS heartbeats (
      id         INTEGER PRIMARY KEY AUTOINCREMENT,
      device     TEXT    NOT NULL,
      server_ts  INTEGER NOT NULL,   -- seconds, server clock (authoritative)
      device_ts  INTEGER,            -- seconds, device clock (0 until NTP sync)
      seq        INTEGER,
      source     TEXT    NOT NULL,   -- 'direct' | 'firebase'
      state      TEXT,               -- online | no_internet | wifi_down
      rssi       INTEGER,
      uptime_s   INTEGER,
      boot_id    INTEGER,
      free_heap  INTEGER,
      fw         TEXT,
      slot       TEXT,
      ip         TEXT,
      relay      INTEGER,
      probe_ms   INTEGER,
      raw        TEXT
    );
    CREATE INDEX IF NOT EXISTS hb_ts     ON heartbeats(server_ts);
    CREATE INDEX IF NOT EXISTS hb_dev_ts ON heartbeats(device, server_ts);

    CREATE TABLE IF NOT EXISTS outages (
      id         INTEGER PRIMARY KEY AUTOINCREMENT,
      device     TEXT    NOT NULL,
      kind       TEXT    NOT NULL,   -- unreachable | no_internet | wifi_down
      start_ts   INTEGER NOT NULL,
      end_ts     INTEGER,            -- NULL while ongoing
      duration_s INTEGER,
      source     TEXT    NOT NULL,   -- 'server' (gap seen) | 'device' (reported)
      note       TEXT
    );
    CREATE INDEX IF NOT EXISTS out_start ON outages(start_ts);
    CREATE UNIQUE INDEX IF NOT EXISTS out_dedupe
      ON outages(device, kind, start_ts, source);

    CREATE TABLE IF NOT EXISTS events (
      id     INTEGER PRIMARY KEY AUTOINCREMENT,
      device TEXT    NOT NULL,
      ts     INTEGER NOT NULL,
      type   TEXT    NOT NULL,       -- boot | fw_change | slot_change
      detail TEXT
    );
    CREATE INDEX IF NOT EXISTS ev_ts ON events(ts);
  `);

  const q = {
    insertHb: db.prepare(`
      INSERT INTO heartbeats
        (device, server_ts, device_ts, seq, source, state, rssi, uptime_s,
         boot_id, free_heap, fw, slot, ip, relay, probe_ms, raw)
      VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)`),

    lastHb: db.prepare(`
      SELECT * FROM heartbeats WHERE device = ? ORDER BY server_ts DESC, id DESC LIMIT 1`),

    lastHbBySource: db.prepare(`
      SELECT source, MAX(server_ts) AS ts FROM heartbeats
      WHERE device = ? GROUP BY source`),

    hbSince: db.prepare(`
      SELECT server_ts, state FROM heartbeats
      WHERE device = ? AND server_ts >= ? ORDER BY server_ts ASC`),

    openOutage: db.prepare(`
      SELECT * FROM outages
      WHERE device = ? AND end_ts IS NULL AND source = 'server'
      ORDER BY start_ts DESC LIMIT 1`),

    startOutage: db.prepare(`
      INSERT OR IGNORE INTO outages (device, kind, start_ts, source, note)
      VALUES (?,?,?,?,?)`),

    closeOutage: db.prepare(`
      UPDATE outages SET end_ts = ?, duration_s = ? - start_ts WHERE id = ?`),

    upsertDeviceOutage: db.prepare(`
      INSERT OR IGNORE INTO outages
        (device, kind, start_ts, end_ts, duration_s, source, note)
      VALUES (?,?,?,?,?, 'device', ?)`),

    /* One query per source: the device reports far more (short) outages than
     * the server detects, so a shared row limit would push older server
     * outages out of long ranges. */
    outagesBetween: db.prepare(`
      SELECT * FROM outages
      WHERE device = ? AND source = 'server'
        AND start_ts < ? AND (end_ts IS NULL OR end_ts > ?)
      ORDER BY start_ts ASC`),

    deviceOutagesBetween: db.prepare(`
      SELECT * FROM outages
      WHERE device = ? AND source = 'device'
        AND start_ts < ? AND (end_ts IS NULL OR end_ts > ?)
      ORDER BY start_ts ASC`),

    insertEvent: db.prepare(`
      INSERT INTO events (device, ts, type, detail) VALUES (?,?,?,?)`),

    recentEvents: db.prepare(`
      SELECT * FROM events WHERE device = ? AND ts >= ? ORDER BY ts DESC LIMIT 200`),

    firstSeen: db.prepare(`
      SELECT MIN(server_ts) AS ts FROM heartbeats WHERE device = ?`),

    pruneHb: db.prepare(`DELETE FROM heartbeats WHERE server_ts < ?`),
  };

  const b = (v) => (v === true ? 1 : v === false ? 0 : v == null ? null : v);
  const n = (v) => (typeof v === 'number' && Number.isFinite(v) ? Math.trunc(v) : null);
  const s = (v) => (v == null ? null : String(v));

  return {
    db,

    /** Record one heartbeat. `source` is 'direct' or 'firebase'. */
    addHeartbeat(device, serverTs, source, hb) {
      q.insertHb.run(
        device, serverTs, n(hb.ts), n(hb.seq), source, s(hb.state), n(hb.rssi),
        n(hb.uptime_s), n(hb.boot_id), n(hb.free_heap), s(hb.fw), s(hb.slot),
        s(hb.ip), b(hb.relay), n(hb.probe_ms), JSON.stringify(hb)
      );
    },

    lastHeartbeat(device) {
      return q.lastHb.get(device) ?? null;
    },

    lastBySource(device) {
      const out = {};
      for (const r of q.lastHbBySource.all(device)) out[r.source] = r.ts;
      return out;
    },

    heartbeatsSince(device, ts) {
      return q.hbSince.all(device, ts);
    },

    currentOutage(device) {
      return q.openOutage.get(device) ?? null;
    },

    beginOutage(device, kind, startTs, note) {
      q.startOutage.run(device, kind, startTs, 'server', note ?? null);
      return q.openOutage.get(device) ?? null;
    },

    endOutage(id, endTs) {
      q.closeOutage.run(endTs, endTs, id);
    },

    /** Outage the device reported about itself once it got back online. */
    addDeviceOutage(device, kind, startTs, endTs, note) {
      q.upsertDeviceOutage.run(
        device, kind, startTs, endTs, Math.max(0, endTs - startTs), note ?? null
      );
    },

    outagesBetween(device, from, to) {
      return q.outagesBetween.all(device, to, from);
    },

    deviceOutagesBetween(device, from, to) {
      return q.deviceOutagesBetween.all(device, to, from);
    },

    addEvent(device, ts, type, detail) {
      q.insertEvent.run(device, ts, type, detail ?? null);
    },

    recentEvents(device, ts) {
      return q.recentEvents.all(device, ts);
    },

    firstSeen(device) {
      return q.firstSeen.get(device)?.ts ?? null;
    },

    prune(olderThanTs) {
      q.pruneHb.run(olderThanTs);
    },
  };
}
