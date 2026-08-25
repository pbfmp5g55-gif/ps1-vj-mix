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
const TICK_HZ      = 20;   // gauge + UDP to the mixer
const SSE_HZ       = 10;   // state back to phones
const PUBLIC_DIR   = path.join(__dirname, 'public');
const VJ_TOKEN     = process.env.CROWD_VJ_TOKEN || crypto.randomBytes(8).toString('hex');

function num(v, d) { const n = parseInt(v, 10); return Number.isFinite(n) ? n : d; }

const gauge = new Gauge();
const sseClients = new Set();
let lastControlSeq = -1;
let lastControlAt = 0;
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
      const now = Date.now() / 1000;
      const claimed = Number(j.taps) || 0;
      statsClaimedTotal += Math.max(0, claimed);
      const got = gauge.tap(j.id, claimed, now, req.socket.remoteAddress || '?');
      statsAcceptedTotal += got;
      res.writeHead(200, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' });
      res.end(JSON.stringify({ ok: true, accepted: got }));
    });
    return;
  }

  // --- audience downlink (state stream) ------------------------------------
  if (p === '/events') {
    res.writeHead(200, {
      'Content-Type': 'text/event-stream; charset=utf-8',
      'Cache-Control': 'no-store',
      'Connection': 'keep-alive',
      'X-Accel-Buffering': 'no',
    });
    res.write(': hello\n\n');
    sseClients.add(res);
    req.on('close', () => sseClients.delete(res));
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
      state: gauge.state(Date.now() / 1000),
      stats: { claimed: statsClaimedTotal, accepted: statsAcceptedTotal },
      mixerControlAgeSec: lastControlAt ? (Date.now() / 1000 - lastControlAt) : null,
    }));
    return;
  }

  if (p === '/health') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({
      ok: true,
      state: gauge.state(Date.now() / 1000),
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
  if (lastControlSeq >= 0 && !pkt.seqNewer(c.seq, lastControlSeq)) return;
  lastControlSeq = c.seq;
  lastControlAt = Date.now() / 1000;
  gauge.holdArmed  = c.holdArmed;
  gauge.windowOpen = c.windowOpen;
});
udpControl.on('error', (e) => console.error('[crowd] control socket:', e.message));
udpOut.on('error', (e) => console.error('[crowd] out socket:', e.message));

// --------------------------------------------------------------------------
// Ticks
// --------------------------------------------------------------------------
let lastTick = Date.now() / 1000;
setInterval(() => {
  const now = Date.now() / 1000;
  const dt = now - lastTick;
  lastTick = now;
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
  const s = gauge.state(Date.now() / 1000);
  const line = 'data: ' + JSON.stringify(s) + '\n\n';
  for (const res of sseClients) {
    try { res.write(line); } catch (e) { sseClients.delete(res); }
  }
}, Math.round(1000 / SSE_HZ));

setInterval(() => gauge.prune(Date.now() / 1000), 60000);

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

udpControl.bind(CONTROL_PORT, '127.0.0.1', () => {
  server.listen(HTTP_PORT, '0.0.0.0', () => {
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
