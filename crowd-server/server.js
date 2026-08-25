'use strict';
// CROWD server — serves the audience page, collects taps, runs the gauge,
// and streams the result to the mixer over loopback UDP.
//
// Zero npm dependencies on purpose: the phone uplink is a plain POST every
// 250 ms and the downlink is Server-Sent Events, both built into Node. A
// WebSocket would need `ws` installed, and "npm install worked on the night"
// is one more thing to fail in a venue.
//
// Design: ../design/CROWD_CONTROL.md

const http = require('http');
const dgram = require('dgram');
const fs = require('fs');
const path = require('path');
const os = require('os');
const crypto = require('crypto');

const { Gauge } = require('./gauge.js');
const pkt = require('./packet.js');

const HTTP_PORT    = num(process.env.CROWD_HTTP_PORT, 8777);
const MIXER_HOST   = process.env.CROWD_MIXER_HOST || '127.0.0.1';
const MIXER_PORT   = num(process.env.CROWD_MIXER_PORT, 8778);
const CONTROL_PORT = num(process.env.CROWD_CONTROL_PORT, 8779);
// Nominal rates. Windows timer granularity is ~15.6 ms, so setInterval(50)
// actually lands around 62 ms and the real cadence measures ~16 Hz (confirmed
// over a 12-minute soak). That is fine: the gauge integrates real elapsed
// time rather than counting ticks, and the mixer holds the last value for
// 400 ms before it starts fading, which is six times the observed gap.
const TICK_HZ      = 20;   // gauge + UDP to the mixer
const SSE_HZ       = 10;   // state back to phones
const PUBLIC_DIR   = path.join(__dirname, 'public');
const VJ_TOKEN     = process.env.CROWD_VJ_TOKEN || crypto.randomBytes(8).toString('hex');
const BIND_ADDR    = process.env.CROWD_BIND || '0.0.0.0';
// How long the mixer can go quiet before we treat it as gone. It sends its
// controls at 10 Hz whenever it is alive.
const MIXER_TIMEOUT_SEC = 3;
const CONTROL_RESYNC_SEC = 2;   // accept any seq after this much silence
const MAX_SSE_CLIENTS = 100;
const MAX_SSE_PER_ADDR = 3;

function num(v, d) { const n = parseInt(v, 10); return Number.isFinite(n) ? n : d; }

// Monotonic seconds. Date.now() goes backwards when Windows syncs its clock,
// which would put lastTapAt in the future and leave devices "active" forever.
const HR0 = process.hrtime.bigint();
function nowSec() { return Number(process.hrtime.bigint() - HR0) / 1e9; }

const gauge = new Gauge();
const sseClients = new Set();
let lastControlSeq = -1;
let lastControlAt = 0;
let mixerOnline = false;
let statsAcceptedTotal = 0;
let statsClaimedTotal = 0;

// --------------------------------------------------------------------------
// HTTP
// --------------------------------------------------------------------------
const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js':   'text/javascript; charset=utf-8',
  '.css':  'text/css; charset=utf-8',
  '.png':  'image/png',
  '.svg':  'image/svg+xml',
};

function isLoopback(req) {
  const a = req.socket.remoteAddress || '';
  return a === '127.0.0.1' || a === '::1' || a === '::ffff:127.0.0.1';
}

function sendFile(res, file) {
  fs.readFile(file, (err, buf) => {
    if (err) { res.writeHead(404); res.end('not found'); return; }
    res.writeHead(200, {
      'Content-Type': MIME[path.extname(file)] || 'application/octet-stream',
      'Cache-Control': 'no-store',
    });
    res.end(buf);
  });
}

function readBody(req, limit, cb) {
  let n = 0;
  const chunks = [];
  req.on('data', (c) => {
    n += c.length;
    if (n > limit) { req.destroy(); return; }
    chunks.push(c);
  });
  req.on('end', () => cb(Buffer.concat(chunks).toString('utf8')));
  req.on('error', () => cb(null));
}

const server = http.createServer((req, res) => {
  const url = new URL(req.url, 'http://localhost');
  const p = url.pathname;

  if (p === '/' || p === '/index.html') return sendFile(res, path.join(PUBLIC_DIR, 'index.html'));
  if (p === '/qr.png') return sendFile(res, path.join(PUBLIC_DIR, 'qr.png'));

  // --- audience uplink -----------------------------------------------------
  if (p === '/tap' && req.method === 'POST') {
    readBody(req, 512, (body) => {
      let j = null;
      try { j = JSON.parse(body || '{}'); } catch (e) { j = null; }
      if (!j || typeof j.id !== 'string' || j.id.length > 64) {
        res.writeHead(400); res.end('{}'); return;
      }
      const now = nowSec();
      const addr = req.socket.remoteAddress || '?';
      const claimed = Number(j.taps) || 0;
      statsClaimedTotal += Math.max(0, claimed);
      const blocked = gauge.deviceBlocked(j.id, addr);
      const got = blocked ? 0 : gauge.tap(j.id, claimed, now, addr);
      statsAcceptedTotal += got;
      res.writeHead(200, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' });
      res.end(JSON.stringify({ ok: !blocked, accepted: got,
                              blocked: blocked ? 'device-limit' : undefined }));
    });
    return;
  }

  // --- audience downlink (state stream) ------------------------------------
  if (p === '/events') {
    // Anyone on the LAN can open these. Cap them, or a single laptop can sit
    // there opening streams until the process runs out of handles.
    const addr = req.socket.remoteAddress || '?';
    let perAddr = 0;
    for (const c of sseClients) if (c.__addr === addr) perAddr++;
    if (sseClients.size >= MAX_SSE_CLIENTS || perAddr >= MAX_SSE_PER_ADDR) {
      res.writeHead(503, { 'Content-Type': 'text/plain' });
      res.end('too many streams');
      return;
    }
    res.writeHead(200, {
      'Content-Type': 'text/event-stream; charset=utf-8',
      'Cache-Control': 'no-store',
      'Connection': 'keep-alive',
      'X-Accel-Buffering': 'no',
    });
    res.write(': hello\n\n');
    res.__addr = addr;
    sseClients.add(res);
    req.on('close', () => sseClients.delete(res));
    res.on('error', () => sseClients.delete(res));
    return;
  }

  // --- VJ tuning page: loopback only, and a token on top -------------------
  if (p === '/vj' || p === '/vj/params') {
    if (!isLoopback(req)) { res.writeHead(403); res.end('VJ page is local-only'); return; }
    if (url.searchParams.get('t') !== VJ_TOKEN) {
      res.writeHead(403); res.end('bad or missing token'); return;
    }
    if (p === '/vj') return sendFile(res, path.join(PUBLIC_DIR, 'vj.html'));
    if (req.method === 'POST') {
      readBody(req, 1024, (body) => {
        let j = null;
        try { j = JSON.parse(body || '{}'); } catch (e) { j = null; }
        if (j) gauge.setParams(j);
        res.writeHead(200, { 'Content-Type': 'application/json' });
        res.end(JSON.stringify({ ok: true, cfg: gauge.cfg }));
      });
      return;
    }
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({
      cfg: gauge.cfg,
      state: gauge.state(nowSec()),
      stats: { claimed: statsClaimedTotal, accepted: statsAcceptedTotal },
      mixerControlAgeSec: lastControlAt ? (nowSec() - lastControlAt) : null,
    }));
    return;
  }

  if (p === '/health') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({
      ok: true,
      state: gauge.state(nowSec()),
      mixerOnline,
      stats: { claimed: statsClaimedTotal, accepted: statsAcceptedTotal },
    }));
    return;
  }

  res.writeHead(404); res.end('not found');
});

// --------------------------------------------------------------------------
// UDP: state out to the mixer, performance controls back from it
// --------------------------------------------------------------------------
const udpOut = dgram.createSocket('udp4');
const udpControl = dgram.createSocket('udp4');

udpControl.on('message', (buf) => {
  const c = pkt.decodeControl(buf);
  if (!c) return;                                   // junk / wrong version
  const t = nowSec();
  // A restarted mixer begins at seq 0 again. Without this, its packets stay
  // "older" than the ones we already saw and its controls are ignored for up
  // to an hour. Any gap in the stream means whatever arrives next is current.
  const resync = lastControlAt === 0 || (t - lastControlAt) > CONTROL_RESYNC_SEC;
  if (!resync && lastControlSeq >= 0 && !pkt.seqNewer(c.seq, lastControlSeq)) return;
  lastControlSeq = c.seq;
  lastControlAt = t;
  gauge.holdArmed  = c.holdArmed;
  gauge.windowOpen = c.windowOpen;
});
udpControl.on('error', (e) => console.error('[crowd] control socket:', e.message));
udpOut.on('error', (e) => console.error('[crowd] out socket:', e.message));

// --------------------------------------------------------------------------
// Ticks
// --------------------------------------------------------------------------
let lastTick = nowSec();
setInterval(() => {
  const now = nowSec();
  const dt = now - lastTick;
  lastTick = now;
  // If the mixer is gone there is nothing on screen to react, so stop taking
  // taps rather than letting a room hammer their phones at a dead projector.
  const wasOnline = mixerOnline;
  mixerOnline = lastControlAt !== 0 && (now - lastControlAt) < MIXER_TIMEOUT_SEC;
  if (wasOnline && !mixerOnline) console.log('[crowd] mixer went quiet');
  if (!wasOnline && mixerOnline) console.log('[crowd] mixer connected');
  if (!mixerOnline) { gauge.windowOpen = false; gauge.holdArmed = false; }
  const r = gauge.tick(dt, now);
  if (r.fired) console.log('[crowd] BURST (active=' + r.rawActive + ')');
  const s = gauge.state(now);
  const buf = pkt.encodeState(s);
  // Nobody listening on the mixer side is the normal case before the mixer
  // starts; on Windows that surfaces as ECONNRESET on a later send. Swallow.
  udpOut.send(buf, MIXER_PORT, MIXER_HOST, () => {});
}, Math.round(1000 / TICK_HZ));

setInterval(() => {
  if (sseClients.size === 0) return;
  const s = gauge.state(nowSec());
  s.mixerOnline = mixerOnline;
  const line = 'data: ' + JSON.stringify(s) + '\n\n';
  for (const res of sseClients) {
    try { res.write(line); } catch (e) { sseClients.delete(res); }
  }
}, Math.round(1000 / SSE_HZ));

setInterval(() => gauge.prune(nowSec()), 60000);

// --------------------------------------------------------------------------
// Start
// --------------------------------------------------------------------------
function lanAddresses() {
  const out = [];
  for (const list of Object.values(os.networkInterfaces())) {
    for (const ni of list || []) {
      if (ni.family === 'IPv4' && !ni.internal) out.push(ni.address);
    }
  }
  return out;
}

function fatal(what, e) {
  console.error('');
  console.error('  CROWD server could not start: ' + what);
  console.error('  ' + (e && e.message ? e.message : e));
  if (e && e.code === 'EADDRINUSE') {
    console.error('  Something is already using that port - another copy of');
    console.error('  this server still running, most likely.');
  }
  console.error('');
  process.exit(1);
}
server.on('error', (e) => fatal('the web server could not listen', e));
udpControl.once('error', (e) => fatal('the control socket could not bind', e));

udpControl.bind(CONTROL_PORT, '127.0.0.1', () => {
  server.listen(HTTP_PORT, BIND_ADDR, () => {
    const addrs = lanAddresses();
    console.log('');
    console.log('  CROWD server up.');
    console.log('  ------------------------------------------------------------');
    for (const a of addrs) console.log('   audience:  http://' + a + ':' + HTTP_PORT + '/');
    if (addrs.length === 0) console.log('   audience:  (no LAN address found - is Wi-Fi on?)');
    console.log('   VJ tuning: http://127.0.0.1:' + HTTP_PORT + '/vj?t=' + VJ_TOKEN);
    console.log('   -> mixer:  UDP ' + MIXER_HOST + ':' + MIXER_PORT +
                '    <- mixer: UDP 127.0.0.1:' + CONTROL_PORT);
    console.log('  ------------------------------------------------------------');
    console.log('  Phones must be on the SAME network as this PC, and Windows');
    console.log('  Firewall must allow Node on a private network.');
    console.log('');
  });
});

process.on('SIGINT', () => { console.log('\n[crowd] bye'); process.exit(0); });
