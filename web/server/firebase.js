/* Polls the device's status node in Firebase RTDB.
 *
 * The status subtree is world-readable by the database rules, so this needs no
 * credentials - writes are what is locked to the device's UID. Only a changed
 * `seq` counts as a fresh observation, so a stale node cannot fake liveness. */

export function makeFirebasePoller({ url, device, intervalMs, onHeartbeat }) {
  const endpoint = `${url.replace(/\/$/, '')}/devices/${device}/status.json`;
  let lastSeq = null;
  let timer = null;
  let failures = 0;

  async function poll() {
    try {
      const ac = new AbortController();
      const t = setTimeout(() => ac.abort(), 10000);
      const res = await fetch(endpoint, { signal: ac.signal, cache: 'no-store' });
      clearTimeout(t);

      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const hb = await res.json();
      failures = 0;
      if (!hb || typeof hb !== 'object') return;

      /* Same sequence number means the device has not written since we last
       * looked - not a new sighting. */
      const seq = Number.isFinite(hb.seq) ? hb.seq : null;
      if (seq == null || seq === lastSeq) return;
      lastSeq = seq;

      onHeartbeat(hb);
    } catch (err) {
      /* Firebase being unreachable is not the device being down - the direct
       * ingest path covers that. Just log, sparsely. */
      if (failures++ % 15 === 0) {
        console.warn(`[firebase] poll failed (${failures}): ${err.message}`);
      }
    }
  }

  return {
    start() {
      poll();
      timer = setInterval(poll, intervalMs);
      return this;
    },
    stop() {
      if (timer) clearInterval(timer);
      timer = null;
    },
  };
}
