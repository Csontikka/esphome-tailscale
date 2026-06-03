/**
 * esphome-tailscale — Anonymous Telemetry Worker (Cloudflare Workers + D1)
 *
 * Backs the ESPHome Tailscale component's anonymous telemetry. Separate worker,
 * separate D1 database and key from the esp32-tailscale-subnet-router worker.
 *
 * Endpoints:
 *   POST /v1/event   — record an event (auth: X-Tlm-Key header == env.TLM_KEY)
 *   GET  /admin      — HTML dashboard (HTTP Basic auth: any user / env.ADMIN_PASS)
 *   GET  /v1/stats   — aggregate JSON
 *   GET  /           — health check
 *
 * Body fields (JSON) — the ENTIRE payload the device sends:
 *   dh  device_hash  (16 hex chars, SHA-256(MAC + salt) truncated; one-way)
 *   v   version      (component version string)
 *   et  event_type   ("boot" | "heartbeat")
 *   ch  chip         ("S3r0" etc)
 *   up  uptime_sec   (int)
 *   bc  boot_count   (int)
 *   rr  reset_reason (int, ESP-IDF esp_reset_reason_t)
 *   ps  psram        (1 | 0 — PSRAM present)
 *   cn  connected    (1 | 0 — connected to the tailnet)
 *   cr  crash_sig    ("task=NAME pc=0xADDR bt=0x..,0x.."; only on a boot after a
 *                     crash — code addresses + task name only, no stack content)
 *
 * Privacy:
 *   The raw client IP is NEVER stored. Coarse geo (country + region) is taken
 *   from the Cloudflare edge connection (request.cf) only — enough to draw a
 *   usage map, with no IP at rest. The device sends no IP either.
 *
 * Timestamps are stored as UNIX seconds (UTC) via strftime('%s','now'); the
 * dashboard renders them in Europe/Budapest.
 */

const ALLOWED_EVENT_TYPES = new Set(['boot', 'heartbeat']);
const MAX_BODY_BYTES = 1024;
const DISPLAY_TZ = 'Europe/Budapest';

/* Legacy bare 16-hex device_hash (firmware <= v0.4.1, no integrity check) is
 * accepted only through 2026-08-31 UTC — a grace window for existing installs to
 * update. From 2026-09-01 only the 18-hex id+check is valid. */
const LEGACY_DH_CUTOFF_MS = Date.UTC(2026, 8, 1); // 2026-09-01T00:00:00Z

function json(obj, status = 200) {
  return new Response(JSON.stringify(obj), {
    status,
    headers: { 'content-type': 'application/json; charset=utf-8', 'cache-control': 'no-store' },
  });
}
const isHex = (s, n) => typeof s === 'string' && s.length === n && /^[0-9a-f]+$/i.test(s);
const asInt = (x) => (Number.isInteger(x) ? x : null);
const asStr = (x, max) => (typeof x === 'string' ? x.slice(0, max) : null);
const asBit = (x) => (x === 1 || x === 0 ? x : (x ? 1 : 0));

/* 2-hex integrity check appended to device_hash: SHA-256(salt + id16)[0],
 * recomputable from the 16-hex id alone (the collector has no MAC), so random/
 * garbage posts can be dropped. Friction, not auth — the firmware + this salt
 * are open source. Must match compute_device_hash() in telemetry.cpp. */
async function dhCheck(id16) {
  const data = new TextEncoder().encode('esphome-tailscale-v1' + id16);
  const d = new Uint8Array(await crypto.subtle.digest('SHA-256', data));
  return d[0].toString(16).padStart(2, '0');
}

/* HTTP Basic gate for admin endpoints (any username; password == env.ADMIN_PASS).
 * Returns a 401/503 Response when unauthorized, or null when OK. */
function checkAdmin(request, env) {
  const realm = { 'WWW-Authenticate': 'Basic realm="esphome-tailscale telemetry"' };
  const auth = request.headers.get('Authorization');
  if (!auth || !auth.startsWith('Basic ')) return new Response('Authentication required', { status: 401, headers: realm });
  if (!env.ADMIN_PASS) return new Response('Admin disabled: set ADMIN_PASS secret.', { status: 503 });
  let pass = '';
  try { const d = atob(auth.slice(6)); pass = d.slice(d.indexOf(':') + 1); }
  catch (e) { return new Response('Invalid auth header', { status: 400 }); }
  if (pass !== env.ADMIN_PASS) return new Response('Forbidden', { status: 401, headers: realm });
  return null;
}

/* Idempotent schema bootstrap — runs on every ingest/admin hit (cheap; D1
 * caches the prepared statements). No separate migration step needed. */
async function ensureSchema(env) {
  await env.DB.batch([
    env.DB.prepare(
      `CREATE TABLE IF NOT EXISTS events (
         id INTEGER PRIMARY KEY AUTOINCREMENT,
         ts INTEGER, country TEXT, region TEXT,
         device_hash TEXT, version TEXT, event_type TEXT, chip TEXT,
         uptime_sec INTEGER, boot_count INTEGER, reset_reason INTEGER,
         psram INTEGER, connected INTEGER, crash_sig TEXT)`),
    env.DB.prepare(`CREATE INDEX IF NOT EXISTS idx_events_ts ON events(ts)`),
    env.DB.prepare(`CREATE INDEX IF NOT EXISTS idx_events_dh ON events(device_hash)`),
    env.DB.prepare(
      `CREATE TABLE IF NOT EXISTS devices (
         device_hash TEXT PRIMARY KEY,
         first_seen INTEGER, last_seen INTEGER,
         last_version TEXT, last_country TEXT, last_region TEXT,
         last_chip TEXT, last_psram INTEGER, total_events INTEGER)`),
    env.DB.prepare(
      `CREATE TABLE IF NOT EXISTS device_versions (
         device_hash TEXT, version TEXT,
         first_seen INTEGER, last_seen INTEGER, count INTEGER,
         PRIMARY KEY (device_hash, version))`),
  ]);
}

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);

    if (request.method === 'GET' && url.pathname === '/') {
      return new Response(
        'esphome-tailscale telemetry worker. POST /v1/event to record, GET /admin for the dashboard.',
        { headers: { 'content-type': 'text/plain; charset=utf-8' } });
    }

    /* ---- ingest ---- */
    if (request.method === 'POST' && url.pathname === '/v1/event') {
      if (env.TLM_KEY && request.headers.get('X-Tlm-Key') !== env.TLM_KEY) {
        return json({ error: 'unauthorized' }, 401);
      }
      if (parseInt(request.headers.get('content-length') || '0', 10) > MAX_BODY_BYTES) {
        return json({ error: 'body too large' }, 413);
      }
      let body;
      try { body = await request.json(); } catch (e) { return json({ error: 'invalid json' }, 400); }

      /* Accept either an 18-char id+check (current firmware) or a bare 16-char id
       * (legacy v0.4.0 builds with no check). For 18 chars, verify the trailing
       * 2-hex check and reject a mismatch; either way we store only the 16-hex id. */
      let dh;
      if (isHex(body.dh, 18)) {
        const id = body.dh.slice(0, 16).toLowerCase();
        if (body.dh.slice(16, 18).toLowerCase() !== await dhCheck(id)) {
          return json({ error: 'failed integrity check' }, 400);
        }
        dh = id;
      } else if (isHex(body.dh, 16)) {
        if (Date.now() >= LEGACY_DH_CUTOFF_MS) {
          return json({ error: 'legacy device_hash retired — update firmware to >= 0.4.2' }, 400);
        }
        dh = body.dh.toLowerCase();
      } else {
        return json({ error: 'invalid device_hash' }, 400);
      }
      if (!ALLOWED_EVENT_TYPES.has(body.et)) return json({ error: 'invalid event_type' }, 400);
      const v  = asStr(body.v, 64);
      const et = body.et;
      const ch = asStr(body.ch, 16);
      const up = asInt(body.up);
      const bc = asInt(body.bc);
      const rr = asInt(body.rr);
      const ps = asBit(body.ps);
      const cn = asBit(body.cn);
      const cr = asStr(body.cr, 256);

      /* Coarse geo from the CF edge — never the raw IP. */
      const country = (request.cf && request.cf.country) || null;
      const region  = (request.cf && request.cf.region)  || null;

      try {
        await ensureSchema(env);
        await env.DB.prepare(
          `INSERT INTO events
             (ts, country, region, device_hash, version, event_type, chip,
              uptime_sec, boot_count, reset_reason, psram, connected, crash_sig)
           VALUES (strftime('%s','now'), ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)`)
          .bind(country, region, dh, v, et, ch, up, bc, rr, ps, cn, cr).run();

        await env.DB.prepare(
          `INSERT INTO devices
             (device_hash, first_seen, last_seen, last_version, last_country,
              last_region, last_chip, last_psram, total_events)
           VALUES (?, strftime('%s','now'), strftime('%s','now'), ?, ?, ?, ?, ?, 1)
           ON CONFLICT(device_hash) DO UPDATE SET
             last_seen     = strftime('%s','now'),
             last_version  = COALESCE(excluded.last_version, devices.last_version),
             last_country  = COALESCE(excluded.last_country, devices.last_country),
             last_region   = COALESCE(excluded.last_region, devices.last_region),
             last_chip     = COALESCE(excluded.last_chip, devices.last_chip),
             last_psram    = COALESCE(excluded.last_psram, devices.last_psram),
             total_events  = devices.total_events + 1`)
          .bind(dh, v, country, region, ch, ps).run();

        /* Per-device version lifecycle: one row per (device, version) with the
         * window it ran. Never pruned (a device runs only a handful of versions),
         * so the upgrade timeline survives even as old `events` rows age out. */
        if (v) {
          await env.DB.prepare(
            `INSERT INTO device_versions (device_hash, version, first_seen, last_seen, count)
             VALUES (?1, ?2, strftime('%s','now'), strftime('%s','now'), 1)
             ON CONFLICT(device_hash, version) DO UPDATE SET
               last_seen = strftime('%s','now'),
               count     = count + 1`)
            .bind(dh, v).run();
        }

        /* Bound per-device storage: keep only the most recent rows per device.
         * Events are NEVER rejected (a crash-looping device's boot reports always
         * land — that's exactly the signal we want); old rows just age out, so one
         * device (crash loop or a single spam source) can't grow the DB unbounded.
         * Abuse gate stays the TLM_KEY + Cloudflare's edge protection. */
        await env.DB.prepare(
          `DELETE FROM events WHERE device_hash = ?1 AND id NOT IN
             (SELECT id FROM events WHERE device_hash = ?1 ORDER BY id DESC LIMIT 200)`)
          .bind(dh).run();
      } catch (e) {
        return json({ error: 'db error', detail: e.message }, 500);
      }
      return json({ ok: true });
    }

    /* ---- admin dashboard ---- */
    if (url.pathname === '/admin' || url.pathname === '/admin/') {
      const unauth = checkAdmin(request, env);
      if (unauth) return unauth;

      try {
        await ensureSchema(env);
        const dev = await env.DB.prepare('SELECT COUNT(*) AS n FROM devices').first();
        const ev  = await env.DB.prepare('SELECT COALESCE(SUM(total_events),0) AS n FROM devices').first();
        const a24 = await env.DB.prepare(`SELECT COUNT(*) AS n FROM devices WHERE last_seen >= strftime('%s','now') - 86400`).first();
        const a7d = await env.DB.prepare(`SELECT COUNT(*) AS n FROM devices WHERE last_seen >= strftime('%s','now') - 604800`).first();
        const byType = await env.DB.prepare(`SELECT event_type, reset_reason, COUNT(*) AS n FROM events GROUP BY event_type, reset_reason ORDER BY n DESC`).all();
        const vers = await env.DB.prepare(`SELECT COALESCE(last_version,'(unknown)') AS v, COUNT(*) AS n FROM devices GROUP BY last_version ORDER BY n DESC LIMIT 20`).all();
        const ctry = await env.DB.prepare(`SELECT COALESCE(last_country,'(unknown)') AS c, COUNT(*) AS n FROM devices GROUP BY last_country ORDER BY n DESC LIMIT 20`).all();
        const psr  = await env.DB.prepare(`SELECT last_psram AS p, COUNT(*) AS n FROM devices GROUP BY last_psram ORDER BY n DESC`).all();
        const recent = await env.DB.prepare(
          `SELECT ts, device_hash, event_type, version, country, region, chip,
                  uptime_sec, boot_count, reset_reason, psram, connected, crash_sig
             FROM events ORDER BY ts DESC LIMIT 1000`).all();
        const topDev = await env.DB.prepare(
          `SELECT device_hash, first_seen, last_seen, last_version, last_country,
                  last_region, last_chip, total_events
             FROM devices ORDER BY total_events DESC LIMIT 1000`).all();
        const schema = await env.DB.prepare(
          `SELECT name, sql FROM sqlite_master WHERE type='table'
             AND name NOT LIKE 'sqlite_%' AND name NOT LIKE '_cf%' ORDER BY name`).all();

        const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) =>
          ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
        const _fmt = new Intl.DateTimeFormat('en-CA', {
          timeZone: DISPLAY_TZ, year: 'numeric', month: '2-digit', day: '2-digit',
          hour: '2-digit', minute: '2-digit', second: '2-digit', hour12: false });
        const fmtTs = (t) => {
          if (t == null || t === '') return '-';
          const d = new Date(Number(t) * 1000);
          if (isNaN(d.getTime())) return '-';
          const p = Object.fromEntries(_fmt.formatToParts(d).map(x => [x.type, x.value]));
          const hh = p.hour === '24' ? '00' : p.hour;
          return `${p.year}-${p.month}-${p.day} ${hh}:${p.minute}:${p.second}`;
        };
        const fmtDur = (s) => s == null ? '-' : s < 60 ? s + 's' : s < 3600 ? Math.floor(s/60)+'m' : s < 86400 ? Math.floor(s/3600)+'h' : Math.floor(s/86400)+'d';
        const fmtGeo = (c, r) => [c, r].filter(Boolean).join(' / ') || '-';
        const yn = (b) => b == null ? '-' : (b ? 'yes' : 'no');
        /* Pretty-print a sqlite_master CREATE statement at render time (DB
         * untouched): break only on top-level commas so strftime('%s','now') and
         * PRIMARY KEY (a, b) inner commas stay intact; one column per line. */
        const fmtDdl = (sql) => {
          if (!sql) return '(no ddl)';
          const i = sql.indexOf('('), j = sql.lastIndexOf(')');
          if (i < 0 || j < 0 || j < i) return sql;
          const head = sql.slice(0, i).trim(), body = sql.slice(i + 1, j);
          const cols = []; let depth = 0, cur = '';
          for (const ch of body) {
            if (ch === '(') depth++; else if (ch === ')') depth--;
            if (ch === ',' && depth === 0) { cols.push(cur); cur = ''; } else cur += ch;
          }
          if (cur.trim()) cols.push(cur);
          return head + ' (\n  ' + cols.map(c => c.replace(/\s+/g, ' ').trim()).filter(Boolean).join(',\n  ') + '\n)';
        };
        const rrLabel = (rr) => {
          const m = {0:['UNKNOWN','mut'],1:['POWERON','ok'],2:['EXT','ok'],3:['SW','mut'],4:['PANIC','err'],5:['INT_WDT','err'],6:['TASK_WDT','err'],7:['WDT','err'],8:['DEEPSLEEP','mut'],9:['BROWNOUT','err'],10:['SDIO','mut']};
          return rr == null ? ['-','mut'] : (m[rr] || [String(rr),'mut']);
        };
        const nowBp = fmtTs(Math.floor(Date.now() / 1000));

        const cards = [['Total devices',dev.n],['Active 24h',a24.n],['Active 7d',a7d.n],['Total events',ev.n]]
          .map(([l,v]) => `<div class="card"><div class="card-v">${esc(v)}</div><div class="card-l">${esc(l)}</div></div>`).join('');
        const typeRows = byType.results.map(r => {
          const [rrL, rrC] = rrLabel(r.reset_reason);
          return `<tr><td><span class="tag tag-${esc(r.event_type)}">${esc(r.event_type)}</span></td><td><span class="rr rr-${rrC}">${esc(rrL)}</span></td><td class="num">${esc(r.n)}</td></tr>`;
        }).join('');
        const verRows  = vers.results.map(r => `<tr><td class="mono">${esc(r.v)}</td><td class="num">${esc(r.n)}</td></tr>`).join('');
        const ctryRows = ctry.results.map(r => `<tr><td>${esc(r.c)}</td><td class="num">${esc(r.n)}</td></tr>`).join('');
        const psrRows  = psr.results.map(r => `<tr><td>${r.p == null ? '(unknown)' : (r.p ? 'PSRAM' : 'no PSRAM')}</td><td class="num">${esc(r.n)}</td></tr>`).join('');
        const recRows = recent.results.map(r => {
          const [rrL, rrC] = rrLabel(r.reset_reason);
          return `<tr${r.crash_sig ? ' class="row-crash"' : ''}>
             <td class="mono">${esc(fmtTs(r.ts))}</td>
             <td class="mono">${esc((r.device_hash||'').slice(0,8))}</td>
             <td><span class="tag tag-${esc(r.event_type)}">${esc(r.event_type)}</span></td>
             <td><span class="rr rr-${rrC}">${esc(rrL)}</span></td>
             <td class="mono">${esc(r.version || '-')}</td>
             <td>${esc(fmtGeo(r.country, r.region))}</td>
             <td class="mono">${esc(r.chip || '-')}</td>
             <td class="num">${esc(r.boot_count ?? '-')}</td>
             <td class="num">${esc(fmtDur(r.uptime_sec))}</td>
             <td>${esc(yn(r.psram))}</td>
             <td>${esc(yn(r.connected))}</td>
             <td class="mono crash-sig" title="${esc(r.crash_sig || '')}">${esc(r.crash_sig ? r.crash_sig.replace(/^task=/, '').slice(0,32) + (r.crash_sig.length > 32 ? '…' : '') : '')}</td>
           </tr>`;
        }).join('');
        const topRows = topDev.results.map(r => `<tr>
             <td class="mono">${esc((r.device_hash||'').slice(0,12))}</td>
             <td class="mono">${esc(fmtTs(r.first_seen))}</td>
             <td class="mono">${esc(fmtTs(r.last_seen))}</td>
             <td class="mono">${esc(r.last_version || '-')}</td>
             <td class="mono">${esc(r.last_chip || '-')}</td>
             <td>${esc(fmtGeo(r.last_country, r.last_region))}</td>
             <td class="num">${esc(r.total_events)}</td>
           </tr>`).join('');
        const schemaRows = schema.results.map(r => `<tr><td class="mono">${esc(r.name)}</td><td class="mono" style="white-space:pre-wrap;font-size:11px">${esc(fmtDdl(r.sql))}</td></tr>`).join('');
        const tsRows = recent.results.slice(0, 5).map(r => `<tr><td class="mono">${esc(r.ts)}</td><td><span class="rr rr-ok">unix-s</span></td><td class="mono">${esc(fmtTs(r.ts))}</td></tr>`).join('');

        const html = `<!doctype html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>esphome-tailscale telemetry — admin</title>
<style>
:root{--bg:#0f1115;--panel:#171a21;--border:#262b36;--fg:#e7ecf3;--mut:#8a93a3;--acc:#4ea1ff;--ok:#3ddc97;--warn:#ffb547;--err:#ff5a5f}
*{box-sizing:border-box}body{margin:0;font:14px/1.45 -apple-system,Segoe UI,Roboto,Arial,sans-serif;background:var(--bg);color:var(--fg)}
.wrap{max-width:1400px;margin:0 auto;padding:0 24px 24px}h1{margin:0;font-size:22px}.sub{color:var(--mut);font-size:13px}
.topbar{position:sticky;top:0;z-index:30;background:var(--bg);border-bottom:1px solid var(--border);display:grid;grid-template-columns:1fr auto 1fr;align-items:center;gap:12px;padding:16px 0;margin-bottom:24px}
.topbar h1{justify-self:start}
.topbar .sub{justify-self:center;background:var(--panel);border:1px solid var(--border);border-radius:99px;padding:6px 14px;max-width:100%;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.topbar .refresh{justify-self:end}
.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:14px;margin-bottom:24px}
.card{background:var(--panel);border:1px solid var(--border);border-radius:10px;padding:18px}
.card-v{font-size:32px;font-weight:600;color:var(--acc)}.card-l{color:var(--mut);font-size:13px;margin-top:4px}
.grid{display:grid;grid-template-columns:1fr 1fr 1fr 1fr;gap:14px;margin-bottom:24px}@media(max-width:1000px){.grid{grid-template-columns:1fr 1fr}}
.panel{background:var(--panel);border:1px solid var(--border);border-radius:10px;overflow:hidden}
.panel h2{margin:0;padding:12px 16px;font-size:12px;font-weight:600;border-bottom:1px solid var(--border);color:var(--mut);text-transform:uppercase;letter-spacing:.05em}
table{width:100%;border-collapse:collapse}th,td{padding:8px 12px;text-align:left;border-bottom:1px solid var(--border);font-size:13px}
th{color:var(--mut);font-weight:500;font-size:12px;text-transform:uppercase}tr:last-child td{border-bottom:0}
.num{text-align:right;font-variant-numeric:tabular-nums}.mono{font-family:ui-monospace,Menlo,Consolas,monospace;font-size:12px}
.tag{display:inline-block;padding:2px 8px;border-radius:99px;font-size:11px;font-weight:600;text-transform:uppercase}
.tag-boot{background:rgba(78,161,255,.15);color:var(--acc)}.tag-heartbeat{background:rgba(61,220,151,.12);color:var(--ok)}
.scroll{overflow-x:auto}tr.row-crash{background:rgba(255,90,95,.07)}tr.row-crash td.crash-sig{color:var(--err);font-weight:600}
.rr{font-size:11px;font-weight:600}.rr-ok{color:var(--ok)}.rr-err{color:var(--err)}.rr-warn{color:var(--warn)}.rr-mut{color:var(--mut)}
a{color:var(--acc);text-decoration:none}.refresh{color:var(--mut);font-size:12px;white-space:nowrap}.foot{color:var(--mut);font-size:11px;text-align:center;margin-top:24px}
.rec-count{font-size:12px;font-weight:400;color:var(--mut);margin-left:8px;text-transform:none;letter-spacing:0}
.rec-controls{padding:8px 16px;color:var(--mut);font-size:12px;border-bottom:1px solid var(--border)}
.rec-controls select{background:var(--bg);color:var(--fg);border:1px solid var(--border);border-radius:4px;padding:2px 6px;font:12px inherit}
.filter-row th{padding:4px 8px;background:rgba(255,255,255,.02)}
.filter-row input{width:100%;box-sizing:border-box;background:var(--bg);color:var(--fg);border:1px solid var(--border);border-radius:4px;padding:3px 6px;font:11px ui-monospace,Menlo,Consolas,monospace}
.filter-row input::placeholder{color:var(--mut);opacity:.5}
thead tr:first-child th{cursor:pointer;user-select:none}thead tr:first-child th:hover{color:var(--fg)}
</style></head><body><div class="wrap">
<div class="topbar">
<h1 style="color:#ffcc33">esphome-tailscale</h1>
<div class="sub">Generated ${esc(nowBp)} <strong>Europe/Budapest</strong> · <a href="/v1/stats">JSON API</a></div>
<a href="" onclick="location.reload();return false" class="refresh">↻ refresh</a>
</div>
<div class="cards">${cards}</div>
<div class="grid">
  <div class="panel"><h2>Events by type + reset</h2><table><thead><tr><th>Type</th><th>Reset</th><th class="num">Count</th></tr></thead><tbody>${typeRows || '<tr><td colspan=3>no data</td></tr>'}</tbody></table></div>
  <div class="panel"><h2>Versions</h2><table><thead><tr><th>Version</th><th class="num">Devices</th></tr></thead><tbody>${verRows || '<tr><td colspan=2>no data</td></tr>'}</tbody></table></div>
  <div class="panel"><h2>Countries</h2><table><thead><tr><th>Country</th><th class="num">Devices</th></tr></thead><tbody>${ctryRows || '<tr><td colspan=2>no data</td></tr>'}</tbody></table></div>
  <div class="panel"><h2>PSRAM</h2><table><thead><tr><th>PSRAM</th><th class="num">Devices</th></tr></thead><tbody>${psrRows || '<tr><td colspan=2>no data</td></tr>'}</tbody></table></div>
</div>
<div class="panel" style="margin-bottom:24px"><h2>Top devices <span id="top-count" class="rec-count"></span></h2>
<div class="rec-controls">Show <select id="top-limit"><option>50</option><option>100</option><option>250</option><option>500</option><option>1000</option></select> rows</div>
<div class="scroll"><table id="top-table">
  <thead><tr><th>Device hash</th><th>First seen</th><th>Last seen</th><th>Version</th><th>Chip</th><th>Geo</th><th class="num">Events</th></tr>
  <tr class="filter-row"><th><input data-col="0" placeholder="filter"></th><th><input data-col="1" placeholder="filter"></th><th><input data-col="2" placeholder="filter"></th><th><input data-col="3" placeholder="filter"></th><th><input data-col="4" placeholder="filter"></th><th><input data-col="5" placeholder="filter"></th><th><input data-col="6" placeholder="filter"></th></tr></thead>
  <tbody>${topRows || '<tr><td colspan=7>no data</td></tr>'}</tbody></table></div></div>
<div class="panel"><h2>Recent activity <span id="rec-count" class="rec-count"></span></h2>
<div class="rec-controls">Show <select id="rec-limit"><option>50</option><option>100</option><option>250</option><option>500</option><option>1000</option></select> rows</div>
<div class="scroll"><table id="rec-table">
  <thead><tr><th>Time (Budapest)</th><th>Device</th><th>Type</th><th>Reset</th><th>Version</th><th>Geo</th><th>Chip</th><th class="num">Boot</th><th class="num">Up</th><th>PSRAM</th><th>Conn</th><th>Crash</th></tr>
  <tr class="filter-row"><th><input data-col="0" placeholder="filter"></th><th><input data-col="1" placeholder="filter"></th><th><input data-col="2" placeholder="filter"></th><th><input data-col="3" placeholder="filter"></th><th><input data-col="4" placeholder="filter"></th><th><input data-col="5" placeholder="filter"></th><th><input data-col="6" placeholder="filter"></th><th><input data-col="7" placeholder="filter"></th><th><input data-col="8" placeholder="filter"></th><th><input data-col="9" placeholder="filter"></th><th><input data-col="10" placeholder="filter"></th><th><input data-col="11" placeholder="filter"></th></tr></thead>
  <tbody>${recRows || '<tr><td colspan=12>no data</td></tr>'}</tbody></table></div></div>
<div class="panel" style="margin-top:24px"><h2>Schema sanity <span class="rec-count">(timestamps are UTC unix seconds)</span></h2>
<div class="scroll"><table>
  <thead><tr><th>Table</th><th>CREATE TABLE</th></tr></thead>
  <tbody>${schemaRows || '<tr><td colspan=2>no data</td></tr>'}</tbody></table></div>
<div class="scroll"><table>
  <thead><tr><th>events.ts (raw)</th><th>Kind</th><th>Rendered (Budapest)</th></tr></thead>
  <tbody>${tsRows || '<tr><td colspan=3>no data</td></tr>'}</tbody></table></div></div>
<div class="foot">esphome-tailscale-telemetry @ Cloudflare Workers · D1 esphome_tailscale_telemetry · DB stores UTC, dashboard renders Europe/Budapest</div>
</div>
<script>
(function(){
  function enhance(tableId, limitId, countId, defCol, defDir){
    var table=document.getElementById(tableId);
    if(!table||!table.tBodies[0])return;
    var tbody=table.tBodies[0];
    var allRows=Array.prototype.slice.call(tbody.rows);
    var limitSel=document.getElementById(limitId);
    var counter=document.getElementById(countId);
    var filters=Array.prototype.slice.call(table.querySelectorAll('.filter-row input'));
    var headers=Array.prototype.slice.call(table.querySelectorAll('thead tr:first-child th'));
    var labels=headers.map(function(th){return th.textContent;});
    var sortCol=defCol, sortDir=defDir;
    function cellText(row,i){var c=row.cells[i];return c?(c.textContent||'').replace(/\\s+/g,' ').trim():'';}
    function num(s){s=(s||'').trim();return /^-?\\d+(\\.\\d+)?$/.test(s)?parseFloat(s):NaN;}
    function apply(){
      var fv=filters.map(function(inp){return (inp.value||'').toLowerCase();});
      var matched=allRows.filter(function(row){
        for(var i=0;i<fv.length;i++){if(fv[i]&&cellText(row,i).toLowerCase().indexOf(fv[i])<0)return false;}
        return true;
      });
      matched.sort(function(a,b){
        var x=cellText(a,sortCol),y=cellText(b,sortCol),nx=num(x),ny=num(y),c;
        if(!isNaN(nx)&&!isNaN(ny))c=nx-ny; else c=(x<y?-1:(x>y?1:0));
        return c*sortDir;
      });
      var limit=parseInt(limitSel.value,10)||50, shown=0;
      matched.forEach(function(row){if(shown<limit){tbody.appendChild(row);row.style.display='';shown++;}else{row.style.display='none';}});
      allRows.forEach(function(row){if(matched.indexOf(row)<0)row.style.display='none';});
      var anyF=fv.some(function(v){return v;});
      counter.textContent='showing '+shown+' of '+matched.length+((anyF&&matched.length!==allRows.length)?(' (filtered from '+allRows.length+')'):'');
      headers.forEach(function(th,i){th.textContent=labels[i]+(i===sortCol?(sortDir>0?' ▲':' ▼'):'');});
    }
    limitSel.addEventListener('change',apply);
    filters.forEach(function(inp){inp.addEventListener('input',apply);});
    headers.forEach(function(th,i){th.addEventListener('click',function(){if(sortCol===i){sortDir=-sortDir;}else{sortCol=i;sortDir=1;}apply();});});
    apply();
  }
  enhance('rec-table','rec-limit','rec-count',0,-1);
  enhance('top-table','top-limit','top-count',6,-1);
})();
</script>
</body></html>`;
        return new Response(html, { headers: { 'content-type': 'text/html; charset=utf-8', 'cache-control': 'no-store' } });
      } catch (e) {
        return new Response('Admin error: ' + e.message, { status: 500 });
      }
    }

    /* ---- aggregate JSON ---- */
    if (request.method === 'GET' && url.pathname === '/v1/stats') {
      const unauth = checkAdmin(request, env);
      if (unauth) return unauth;
      try {
        await ensureSchema(env);
        const dev = await env.DB.prepare('SELECT COUNT(*) AS n FROM devices').first();
        const ev  = await env.DB.prepare('SELECT COALESCE(SUM(total_events),0) AS n FROM devices').first();
        const a24 = await env.DB.prepare(`SELECT COUNT(*) AS n FROM devices WHERE last_seen >= strftime('%s','now') - 86400`).first();
        const byType = await env.DB.prepare('SELECT event_type, COUNT(*) AS n FROM events GROUP BY event_type').all();
        const vers = await env.DB.prepare(`SELECT last_version AS v, COUNT(*) AS n FROM devices WHERE last_version IS NOT NULL GROUP BY last_version ORDER BY n DESC LIMIT 20`).all();
        const ctry = await env.DB.prepare(`SELECT last_country AS c, COUNT(*) AS n FROM devices WHERE last_country IS NOT NULL GROUP BY last_country ORDER BY n DESC LIMIT 20`).all();
        return json({
          devices_total: dev.n, devices_active_24h: a24.n, events_total: ev.n,
          events_by_type: byType.results, versions: vers.results, countries: ctry.results,
        });
      } catch (e) {
        return json({ error: 'db error', detail: e.message }, 500);
      }
    }

    return new Response('Not Found', { status: 404 });
  },
};
