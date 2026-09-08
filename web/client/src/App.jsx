import { useCallback, useEffect, useLayoutEffect, useRef, useState } from 'react';

/* ── formatting ──────────────────────────────────────────────────────────── */

const RANGES = [
  ['1h', '1 hour'],
  ['24h', '24 hours'],
  ['7d', '7 days'],
  ['30d', '30 days'],
];

function dur(s) {
  if (s == null) return '-';
  s = Math.max(0, Math.round(s));
  if (s < 60) return `${s}s`;
  const d = Math.floor(s / 86400);
  const h = Math.floor((s % 86400) / 3600);
  const m = Math.floor((s % 3600) / 60);
  if (d) return `${d}d ${h}h`;
  if (h) return `${h}h ${m}m`;
  return `${m}m ${s % 60}s`;
}

const timeFmt = new Intl.DateTimeFormat(undefined, { hour: '2-digit', minute: '2-digit' });
const dateTimeFmt = new Intl.DateTimeFormat(undefined, {
  month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit',
});
const dayFmt = new Intl.DateTimeFormat(undefined, { month: 'short', day: 'numeric' });

const asTime = (ts) => (ts ? timeFmt.format(new Date(ts * 1000)) : '-');
const asDateTime = (ts) => (ts ? dateTimeFmt.format(new Date(ts * 1000)) : '-');
const asDay = (ts) => (ts ? dayFmt.format(new Date(ts * 1000)) : '-');
const pct = (v, dp = 2) => (v == null ? '-' : `${v.toFixed(dp)}%`);

const CAUSE_LABEL = {
  no_internet: 'Router up, no internet',
  unreachable: 'No contact',
  wifi_down: 'Wi-Fi down',
};

/* ── data ────────────────────────────────────────────────────────────────── */

async function getJson(url) {
  const res = await fetch(url, { cache: 'no-store' });
  if (!res.ok) throw new Error(`HTTP ${res.status}`);
  return res.json();
}

function useWidth() {
  const ref = useRef(null);
  const [w, setW] = useState(0);
  useLayoutEffect(() => {
    const el = ref.current;
    if (!el) return;
    const ro = new ResizeObserver(([e]) => setW(e.contentRect.width));
    ro.observe(el);
    setW(el.clientWidth);
    return () => ro.disconnect();
  }, []);
  return [ref, w];
}

function useTooltip() {
  const [tip, setTip] = useState(null);
  const show = useCallback((e, content) => {
    setTip({ x: e.clientX, y: e.clientY, content });
  }, []);
  const hide = useCallback(() => setTip(null), []);
  return { tip, show, hide };
}

function Tooltip({ tip }) {
  if (!tip) return null;
  const pad = 14;
  const style = {
    left: Math.min(tip.x + pad, window.innerWidth - 270),
    top: Math.max(8, tip.y - 12),
  };
  return <div className="tip" style={style}>{tip.content}</div>;
}

/* A bar anchored to the baseline with only its data-end rounded. */
function topRounded(x, yTop, w, h, r) {
  const rr = Math.max(0, Math.min(r, w / 2, h));
  const yb = yTop + h;
  return `M${x},${yb} L${x},${yTop + rr} Q${x},${yTop} ${x + rr},${yTop}` +
         ` L${x + w - rr},${yTop} Q${x + w},${yTop} ${x + w},${yTop + rr}` +
         ` L${x + w},${yb} Z`;
}

/* ── status hero ─────────────────────────────────────────────────────────── */

function Hero({ status }) {
  if (!status) return <div className="card"><div className="empty">Loading…</div></div>;

  const up = status.online;
  const never = status.last_seen == null;
  const cls = never ? 'unknown' : up ? 'up' : 'down';
  const cause = status.device_info?.state;

  return (
    <div className="card">
      <div className="hero">
        <div className={`glyph ${cls}`} aria-hidden="true">{never ? '?' : up ? '✓' : '✕'}</div>
        <div>
          <div className="headline">
            {never ? 'No data yet' : up ? 'Internet is up' : 'Internet is down'}
          </div>
          <div className="detail">
            {never
              ? 'Waiting for the first heartbeat from the device.'
              : up
                ? `Last heartbeat ${status.seconds_since}s ago${cause && cause !== 'online' ? ` · device reports ${cause}` : ''}`
                : `Down for ${dur(status.current_outage?.duration ?? status.seconds_since)} · last seen ${asDateTime(status.last_seen)}`}
          </div>
        </div>
        <div className="spacer" />
        <div style={{ textAlign: 'right' }}>
          <div className="detail" style={{ fontSize: 12 }}>Heartbeat paths</div>
          <div className="detail">
            <SourceDot label="direct" ts={status.sources?.direct} now={status.server_ts} gap={status.gap_seconds} />
            {' '}
            <SourceDot label="firebase" ts={status.sources?.firebase} now={status.server_ts} gap={status.gap_seconds} />
          </div>
        </div>
      </div>
    </div>
  );
}

function SourceDot({ label, ts, now, gap }) {
  const ok = ts != null && now - ts <= gap;
  return (
    <span className="pill" style={{ marginRight: 12 }} title={ts ? `last ${asDateTime(ts)}` : 'never'}>
      <span className="dot" style={{ background: ok ? 'var(--good)' : 'var(--critical)' }} />
      {label} {ok ? 'ok' : 'stale'}
    </span>
  );
}

/* ── KPI row ─────────────────────────────────────────────────────────────── */

function Kpis({ stats, range }) {
  const w = stats?.ranges?.[range];
  const label = RANGES.find(([k]) => k === range)?.[1] ?? range;
  return (
    <div className="kpis" role="group" aria-label={`Statistics for the last ${label}`}>
      <div className="kpi">
        <div className="label">Uptime</div>
        <div className="value">{pct(w?.uptime_pct)}</div>
        <div className="foot">last {label}{w?.partial ? ' (partial)' : ''}</div>
      </div>
      <div className="kpi">
        <div className="label">Outages</div>
        <div className="value">{w?.outages ?? '-'}</div>
        <div className="foot">distinct drops</div>
      </div>
      <div className="kpi">
        <div className="label">Total downtime</div>
        <div className="value">{dur(w?.down_s)}</div>
        <div className="foot">of {dur(w?.covered_s)} watched</div>
      </div>
      <div className="kpi">
        <div className="label">Longest drop</div>
        <div className="value">{w?.longest_s ? dur(w.longest_s) : '-'}</div>
        <div className="foot">{w?.mtbf_s ? `~${dur(w.mtbf_s)} between` : 'single or none'}</div>
      </div>
    </div>
  );
}

/* ── uptime timeline ─────────────────────────────────────────────────────── */

/* State is encoded by segment HEIGHT first (10 / 30 / 44 px, rising with
 * severity) and colour second. Green-vs-red is indistinguishable under
 * deuteranopia, so colour alone is never the carrier here. */
const BAND = {
  up:          { h: 10, fill: 'var(--good)',     label: 'Up' },
  no_internet: { h: 30, fill: 'var(--serious)',  label: CAUSE_LABEL.no_internet },
  unreachable: { h: 44, fill: 'var(--critical)', label: CAUSE_LABEL.unreachable },
  wifi_down:   { h: 38, fill: 'var(--warning)',  label: CAUSE_LABEL.wifi_down },
  unknown:     { h: 4,  fill: 'var(--axis)',     label: 'No data' },
};

function buildBands(timeline) {
  if (!timeline) return [];
  const { from, to, known_from: known, segments } = timeline;
  const out = [];
  if (known > from) out.push({ state: 'unknown', from, to: known });

  let cursor = known;
  const sorted = [...segments].sort((a, b) => a.clipped_start - b.clipped_start);
  for (const s of sorted) {
    const a = Math.max(s.clipped_start, cursor);
    const b = Math.max(s.clipped_end, a);
    if (a > cursor) out.push({ state: 'up', from: cursor, to: a });
    if (b > a || s.ongoing) {
      out.push({ state: BAND[s.cause] ? s.cause : 'unreachable', from: a, to: Math.max(b, a + 1), outage: s });
    }
    cursor = Math.max(cursor, b);
  }
  if (cursor < to) out.push({ state: 'up', from: cursor, to });
  return out;
}

function Timeline({ timeline, tip }) {
  const [ref, w] = useWidth();
  const bands = buildBands(timeline);
  const H = 76;
  const BASE = 60;

  const span = timeline ? Math.max(1, timeline.to - timeline.from) : 1;
  const x = (t) => ((t - timeline.from) / span) * w;

  return (
    <div className="card">
      <h2>Connection timeline</h2>
      <p className="caption">
        Taller and redder means worse. Hover any segment for exact times.
      </p>
      <div className="timeline" ref={ref}>
        {w > 0 && timeline && (
          <svg height={H} role="img" aria-label="Connection state over the selected period">
            <line x1="0" y1={BASE + 0.5} x2={w} y2={BASE + 0.5} stroke="var(--axis)" strokeWidth="1" />
            {bands.map((b, i) => {
              const spec = BAND[b.state];
              const x0 = x(b.from);
              const raw = x(b.to) - x0;
              const isDown = b.state !== 'up' && b.state !== 'unknown';
              const wid = Math.max(isDown ? 2.5 : 0.5, raw - (bands.length > 1 ? 2 : 0));
              if (wid <= 0) return null;
              return (
                <path
                  key={i}
                  d={topRounded(x0 + 1, BASE - spec.h, wid, spec.h, 4)}
                  fill={spec.fill}
                  onMouseMove={(e) => tip.show(e, (
                    <>
                      <b>{spec.label}</b><br />
                      <span className="k">from</span> {asDateTime(b.from)}<br />
                      <span className="k">to</span> {b.outage?.ongoing ? 'now (ongoing)' : asDateTime(b.to)}<br />
                      <span className="k">lasted</span> {dur(b.to - b.from)}
                      {b.outage?.device_confirmed === false && <><br /><span className="k">cause unconfirmed</span></>}
                    </>
                  ))}
                  onMouseLeave={tip.hide}
                />
              );
            })}
          </svg>
        )}
      </div>
      <div className="axis-row">
        <span>{timeline ? asDateTime(timeline.from) : ''}</span>
        <span>{timeline ? asDateTime(timeline.to) : ''}</span>
      </div>
      <div className="legend">
        {['up', 'no_internet', 'unreachable', 'unknown'].map((k) => (
          <div className="item" key={k}>
            <span className="swatch">
              <i style={{ height: Math.round(BAND[k].h / 2), background: BAND[k].fill }} />
            </span>
            {BAND[k].label}
          </div>
        ))}
      </div>
    </div>
  );
}

/* ── downtime per day ────────────────────────────────────────────────────── */

function DailyBars({ daily, tip }) {
  const [ref, w] = useWidth();
  const H = 150, PAD_B = 22, PAD_T = 10;
  const rows = daily ?? [];
  const mins = rows.map((d) => (d.covered_s > 0 ? d.down_s / 60 : 0));
  const max = Math.max(10, Math.ceil(Math.max(0, ...mins) / 10) * 10);
  const plot = H - PAD_B - PAD_T;
  const step = rows.length ? w / rows.length : 0;
  const bw = Math.max(2, step - 3);

  return (
    <div className="card">
      <h2>Downtime per day</h2>
      <p className="caption">Minutes offline each day for the last 30 days.</p>
      <div className="bars" ref={ref}>
        {w > 0 && (
          <svg height={H} role="img" aria-label="Minutes of downtime per day over the last 30 days">
            {[0, 0.5, 1].map((f) => (
              <g key={f}>
                <line x1="0" x2={w} y1={PAD_T + plot * (1 - f)} y2={PAD_T + plot * (1 - f)}
                      stroke="var(--grid)" strokeWidth="1" />
                <text x="0" y={PAD_T + plot * (1 - f) - 4} fill="var(--muted)" fontSize="10"
                      style={{ fontVariantNumeric: 'tabular-nums' }}>
                  {Math.round(max * f)}m
                </text>
              </g>
            ))}
            {rows.map((d, i) => {
              const v = d.covered_s > 0 ? d.down_s / 60 : null;
              const h = v == null ? 2 : Math.max(v > 0 ? 3 : 1, (v / max) * plot);
              const x0 = i * step + 1.5;
              return (
                <path
                  key={d.day}
                  d={topRounded(x0, PAD_T + plot - h, bw, h, 4)}
                  fill={v == null ? 'var(--axis)' : 'var(--series-1)'}
                  onMouseMove={(e) => tip.show(e, (
                    <>
                      <b>{asDay(d.day)}</b><br />
                      {d.covered_s === 0 ? <span className="k">not monitored</span> : (
                        <>
                          <span className="k">downtime</span> {dur(d.down_s)}<br />
                          <span className="k">uptime</span> {pct(d.uptime_pct)}<br />
                          <span className="k">outages</span> {d.outages}
                        </>
                      )}
                    </>
                  ))}
                  onMouseLeave={tip.hide}
                />
              );
            })}
            <line x1="0" x2={w} y1={PAD_T + plot + 0.5} y2={PAD_T + plot + 0.5} stroke="var(--axis)" />
            {rows.length > 0 && (
              <>
                <text x="0" y={H - 6} fill="var(--muted)" fontSize="10">{asDay(rows[0].day)}</text>
                <text x={w} y={H - 6} fill="var(--muted)" fontSize="10" textAnchor="end">
                  {asDay(rows[rows.length - 1].day)}
                </text>
              </>
            )}
          </svg>
        )}
      </div>
    </div>
  );
}

/* ── outage table ────────────────────────────────────────────────────────── */

function Outages({ outages }) {
  const rows = outages ?? [];
  return (
    <div className="card">
      <h2>Outage log</h2>
      <p className="caption">
        Every drop, newest first. &ldquo;Router up, no internet&rdquo; means the ESP32 stayed
        joined to your Wi-Fi the whole time and told us so once it recovered - that is
        the ISP&rsquo;s side, not yours.
      </p>
      {rows.length === 0 ? (
        <div className="empty">No outages recorded in this period.</div>
      ) : (
        <div className="table-scroll">
          <table>
            <thead>
              <tr>
                <th>Started</th>
                <th>Ended</th>
                <th className="num">Duration</th>
                <th>Cause</th>
                <th>Confirmed</th>
              </tr>
            </thead>
            <tbody>
              {rows.map((o) => (
                <tr key={o.id}>
                  <td>{asDateTime(o.start)}</td>
                  <td>{o.ongoing ? 'ongoing' : asDateTime(o.end)}</td>
                  <td className="num">{dur(o.duration)}</td>
                  <td>
                    <span className={`pill cause-${o.cause}`}>
                      <span className="dot" />
                      {CAUSE_LABEL[o.cause] ?? o.cause}
                    </span>
                  </td>
                  <td>{o.device_confirmed ? 'by device' : 'server only'}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      )}
    </div>
  );
}

/* ── device panel ────────────────────────────────────────────────────────── */

function Device({ status, events }) {
  const d = status?.device_info;
  return (
    <div className="card">
      <h2>Device</h2>
      <p className="caption">
        Reboots matter: a device that restarted looks the same from outside as one
        that lost its connection.
      </p>
      <div className="dl">
        <div><dt>Firmware</dt><dd>{d?.fw ?? '-'}</dd></div>
        <div><dt>Slot</dt><dd>{d?.slot ?? '-'}</dd></div>
        <div><dt>Address</dt><dd>{d?.ip ?? '-'}</dd></div>
        <div><dt>Signal</dt><dd>{d?.rssi != null ? `${d.rssi} dBm` : '-'}</dd></div>
        <div><dt>Device uptime</dt><dd>{dur(d?.uptime_s)}</dd></div>
        <div><dt>Free heap</dt><dd>{d?.free_heap != null ? `${Math.round(d.free_heap / 1024)} KB` : '-'}</dd></div>
        <div><dt>Boot count</dt><dd>{d?.boot_id ?? '-'}</dd></div>
        <div><dt>Last reset</dt><dd>{d?.reset_reason ?? '-'}</dd></div>
        <div><dt>Wi-Fi reconnects</dt><dd>{d?.wifi_reconnects ?? '-'}</dd></div>
        <div><dt>Probe latency</dt><dd>{d?.probe_ms != null ? `${d.probe_ms} ms` : '-'}</dd></div>
        <div><dt>Relay</dt><dd>{d == null ? '-' : d.relay ? 'on' : 'off'}</dd></div>
        <div><dt>Watching since</dt><dd>{asDateTime(status?.monitoring_since)}</dd></div>
      </div>

      {events && events.length > 0 && (
        <>
          <h2 style={{ marginTop: 26 }}>Recent device events</h2>
          <div className="table-scroll">
            <table>
              <thead><tr><th>When</th><th>Event</th><th>Detail</th></tr></thead>
              <tbody>
                {events.slice(0, 12).map((e) => (
                  <tr key={e.id}>
                    <td>{asDateTime(e.ts)}</td>
                    <td>{e.type}</td>
                    <td style={{ whiteSpace: 'normal' }}>{e.detail}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </>
      )}
    </div>
  );
}

/* ── app ─────────────────────────────────────────────────────────────────── */

export default function App() {
  const [range, setRange] = useState('24h');
  const [status, setStatus] = useState(null);
  const [stats, setStats] = useState(null);
  const [timeline, setTimeline] = useState(null);
  const [outages, setOutages] = useState(null);
  const [events, setEvents] = useState(null);
  const [err, setErr] = useState(null);
  const [stamp, setStamp] = useState(null);
  const tip = useTooltip();

  const load = useCallback(async () => {
    try {
      const [s, st, tl, og, ev] = await Promise.all([
        getJson('/api/status'),
        getJson('/api/stats'),
        getJson(`/api/timeline?range=${range}`),
        getJson(`/api/outages?range=${range === '1h' ? '24h' : range}`),
        getJson('/api/events'),
      ]);
      setStatus(s); setStats(st); setTimeline(tl); setOutages(og.outages); setEvents(ev.events);
      setErr(null); setStamp(new Date());
    } catch (e) {
      setErr(e.message);
    }
  }, [range]);

  useEffect(() => {
    load();
    const t = setInterval(load, 15000);
    return () => clearInterval(t);
  }, [load]);

  return (
    <div className="wrap">
      <header className="top">
        <div>
          <h1>Internet uptime</h1>
          <div className="sub">
            Watched from the outside by an ESP32 on your home Wi-Fi, reporting once a minute.
          </div>
        </div>
      </header>

      {err && (
        <div className="card" style={{ borderColor: 'var(--critical)' }}>
          <b>Cannot reach the monitor API</b>
          <div className="empty">{err} - the dashboard itself may be fine; retrying every 15s.</div>
        </div>
      )}

      <Hero status={status} />

      <div className="filters">
        <div className="seg" role="group" aria-label="Time range">
          {RANGES.map(([k, label]) => (
            <button key={k} aria-pressed={range === k} onClick={() => setRange(k)}>{label}</button>
          ))}
        </div>
        <div className="grow" />
        <div className="stamp">{stamp ? `updated ${asTime(Math.floor(stamp.getTime() / 1000))}` : ''}</div>
      </div>

      <Kpis stats={stats} range={range} />
      <div style={{ height: 16 }} />

      <Timeline timeline={timeline} tip={tip} />
      <DailyBars daily={stats?.daily} tip={tip} />
      <Outages outages={outages} />
      <Device status={status} events={events} />

      <footer className="foot">
        esp32-home &middot; heartbeats via direct POST and Firebase Realtime Database
      </footer>

      <Tooltip tip={tip.tip} />
    </div>
  );
}
