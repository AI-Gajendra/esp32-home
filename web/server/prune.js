/* Retention for the Firebase Realtime Database.
 *
 * The device appends outage events under /devices/<id>/events keyed by their
 * start timestamp. Nothing in Firebase expires on its own, so without this the
 * subtree grows forever. SQLite on this box is the long-term record; Firebase
 * only needs a recent window for redundancy.
 *
 * Keys are read with ?shallow=true, which returns just the key names rather
 * than the whole subtree - so the cost of a sweep does not grow with the size
 * of the documents, only with their number.
 */

const MIN_PLAUSIBLE_TS = 1600000000;   // 2020-09; anything older is not a timestamp

async function fbFetch(url, init = {}, timeoutMs = 15000) {
  const ac = new AbortController();
  const t = setTimeout(() => ac.abort(), timeoutMs);
  try {
    const res = await fetch(url, { ...init, signal: ac.signal, cache: 'no-store' });
    if (!res.ok) throw new Error(`HTTP ${res.status}`);
    return res;
  } finally {
    clearTimeout(t);
  }
}

/**
 * Delete timestamp-keyed children older than the cutoff.
 * Returns {scanned, deleted, skipped, errors}.
 */
export async function prunePath({ dbUrl, secret, path, cutoffTs, dryRun = false }) {
  const base = `${dbUrl.replace(/\/$/, '')}/${path}.json`;
  const auth = secret ? `auth=${encodeURIComponent(secret)}` : '';

  const res = await fbFetch(`${base}?shallow=true&${auth}`);
  const keys = await res.json();

  const out = { scanned: 0, deleted: 0, skipped: 0, errors: 0 };
  if (!keys || typeof keys !== 'object') return out;

  for (const key of Object.keys(keys)) {
    out.scanned++;

    /* Only ever delete something whose key is unambiguously an old unix
     * timestamp. A push id, a stray node, or a key we cannot parse is left
     * alone - this job must not be able to eat data it does not understand. */
    const ts = Number(key);
    if (!Number.isInteger(ts) || ts < MIN_PLAUSIBLE_TS) { out.skipped++; continue; }
    if (ts >= cutoffTs) { out.skipped++; continue; }

    if (dryRun) { out.deleted++; continue; }
    try {
      await fbFetch(`${base.replace(/\.json$/, '')}/${key}.json?${auth}`, { method: 'DELETE' });
      out.deleted++;
    } catch (err) {
      out.errors++;
      console.warn(`[prune] ${path}/${key}: ${err.message}`);
    }
  }
  return out;
}

export function makePruner({ dbUrl, secret, device, retainDays, intervalMs }) {
  const paths = [`devices/${device}/events`];
  let timer = null;

  async function sweep({ dryRun = false } = {}) {
    const cutoff = Math.floor(Date.now() / 1000) - retainDays * 86400;
    const totals = { scanned: 0, deleted: 0, skipped: 0, errors: 0 };

    for (const path of paths) {
      try {
        const r = await prunePath({ dbUrl, secret, path, cutoffTs: cutoff, dryRun });
        for (const k of Object.keys(totals)) totals[k] += r[k];
      } catch (err) {
        totals.errors++;
        console.warn(`[prune] ${path}: ${err.message}`);
      }
    }

    if (totals.deleted || totals.errors) {
      console.log(`[prune] retain ${retainDays}d (before ` +
        `${new Date(cutoff * 1000).toISOString()}): ` +
        `scanned ${totals.scanned}, deleted ${totals.deleted}, ` +
        `kept ${totals.skipped}, errors ${totals.errors}`);
    }
    return { ...totals, cutoff, retain_days: retainDays };
  }

  return {
    sweep,
    start() {
      if (!secret) {
        console.log('[prune] FIREBASE_DB_SECRET not set - retention disabled ' +
                    '(deleting needs write access)');
        return this;
      }
      sweep();
      timer = setInterval(sweep, intervalMs);
      console.log(`[prune] retention ${retainDays}d, sweeping every ` +
                  `${Math.round(intervalMs / 60000)}min`);
      return this;
    },
    stop() {
      if (timer) clearInterval(timer);
      timer = null;
    },
  };
}
