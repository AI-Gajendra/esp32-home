/* Remote relay control for a device that lives behind NAT.
 *
 * This server can never open a connection to the ESP32 - traffic only ever
 * flows outward from it. So a command is parked here and handed to the device
 * in the reply to its next heartbeat, which means the relay follows within one
 * heartbeat interval rather than instantly. That latency is the price of not
 * holding a connection open around the clock; it is deliberate, not an
 * oversight.
 *
 * The PIN guards this and only this. Everything else on the site stays public
 * and read-only.
 */

const nowS = () => Math.floor(Date.now() / 1000);

/** Length-independent comparison, so a wrong PIN cannot be found a digit at a
 *  time by timing the response. */
function constantTimeEqual(a, b) {
  const sa = String(a ?? '');
  const sb = String(b ?? '');
  let diff = sa.length ^ sb.length;
  for (let i = 0; i < Math.max(sa.length, sb.length); i++) {
    diff |= sa.charCodeAt(i % (sa.length || 1)) ^ sb.charCodeAt(i % (sb.length || 1));
  }
  return diff === 0;
}

export function makeRelayControl({
  pin,
  ttlSeconds = 600,
  maxFails = 5,
  lockoutMs = 15 * 60 * 1000,
}) {
  let desired = null;          // true/false while a command is outstanding
  let commandedAt = 0;
  let source = null;
  let lastReported = null;     // what the device last told us it is doing
  let lastReportedAt = 0;
  let lastAppliedAt = 0;

  /* A six-digit PIN is a million guesses, which is nothing for a script over
   * the public internet. Throttling per address is what actually makes it
   * safe, so it is not optional. */
  const fails = new Map();     // ip -> { count, lockedUntil }

  function throttleState(ip) {
    const rec = fails.get(ip);
    if (!rec) return { locked: false, retryAfter: 0 };
    if (rec.lockedUntil > Date.now()) {
      return { locked: true, retryAfter: Math.ceil((rec.lockedUntil - Date.now()) / 1000) };
    }
    if (rec.lockedUntil && rec.lockedUntil <= Date.now()) fails.delete(ip);
    return { locked: false, retryAfter: 0 };
  }

  function noteFailure(ip) {
    const rec = fails.get(ip) ?? { count: 0, lockedUntil: 0 };
    rec.count++;
    if (rec.count >= maxFails) {
      rec.lockedUntil = Date.now() + lockoutMs;
      rec.count = 0;
      console.warn(`[relay] ${ip} locked out for ${lockoutMs / 60000}min after ${maxFails} bad PINs`);
    }
    fails.set(ip, rec);
  }

  /** A command nobody could deliver in time is dropped rather than applied
   *  late: a relay flipping hours after you asked, because the device only
   *  just came back, is worse than the command being lost. */
  function expireIfStale() {
    if (desired !== null && nowS() - commandedAt > ttlSeconds) {
      console.log(`[relay] command "${desired ? 'on' : 'off'}" expired after ${ttlSeconds}s undelivered`);
      desired = null;
      source = null;
    }
  }

  return {
    /** Returns {ok} or {ok:false, status, error, retryAfter}. */
    authorize(ip, given) {
      if (!pin) return { ok: false, status: 503, error: 'relay control is not configured' };

      const t = throttleState(ip);
      if (t.locked) {
        return { ok: false, status: 429, error: 'too many wrong PINs', retryAfter: t.retryAfter };
      }
      if (!constantTimeEqual(pin, given)) {
        noteFailure(ip);
        return { ok: false, status: 401, error: 'wrong PIN' };
      }
      fails.delete(ip);
      return { ok: true };
    },

    /** `want` is 'on' | 'off' | 'toggle'. Toggle is resolved here, against the
     *  last state the device reported, so the device is always handed an
     *  absolute value - a late-delivered "toggle" would otherwise mean
     *  whatever the state happened to be by then. */
    command(want, by = 'dashboard') {
      let target;
      if (want === 'on') target = true;
      else if (want === 'off') target = false;
      else if (want === 'toggle') target = !(desired ?? lastReported ?? false);
      else return { ok: false, error: 'state must be on, off or toggle' };

      desired = target;
      commandedAt = nowS();
      source = by;
      console.log(`[relay] commanded ${target ? 'ON' : 'OFF'} by ${by}`);
      return { ok: true, ...this.snapshot() };
    },

    /** Called for every heartbeat, from either sink. */
    noteHeartbeat(relayState) {
      if (typeof relayState !== 'boolean') return;
      lastReported = relayState;
      lastReportedAt = nowS();

      expireIfStale();
      if (desired !== null && desired === relayState) {
        console.log(`[relay] device confirmed ${relayState ? 'ON' : 'OFF'}`);
        desired = null;
        source = null;
        lastAppliedAt = nowS();
      }
    },

    /** What to hand the device in its heartbeat reply: true, false, or null
     *  when there is nothing outstanding. */
    pendingForDevice() {
      expireIfStale();
      return desired;
    },

    snapshot(nextHeartbeatEta = null) {
      expireIfStale();
      return {
        state: lastReported,
        reported_at: lastReportedAt || null,
        desired,
        pending: desired !== null,
        commanded_at: desired !== null ? commandedAt : null,
        commanded_by: source,
        applied_at: lastAppliedAt || null,
        expires_in_s: desired !== null ? Math.max(0, ttlSeconds - (nowS() - commandedAt)) : null,
        eta_s: nextHeartbeatEta,
        configured: Boolean(pin),
      };
    },
  };
}
