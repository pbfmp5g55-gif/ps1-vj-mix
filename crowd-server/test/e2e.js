'use strict';
// End-to-end test: starts the real server, talks to it over real HTTP, and
// listens on a real UDP socket in the mixer's place. Uses non-default ports
// so it cannot collide with a server the VJ has running.
//
//   node crowd-server/test/e2e.js

const { spawn } = require('child_process');
const dgram = require('dgram');
const path = require('path');
const assert = require('assert');
const pkt = require('../packet.js');

const HTTP = 8877, MIXER = 8878, CONTROL = 8879;
const BASE = 'http://127.0.0.1:' + HTTP;
const TOKEN = 'testtoken';

let passed = 0, failed = 0;
function check(name, cond, extra) {
  if (cond) { passed++; console.log('  ok   ' + name); }
  else { failed++; console.log('  FAIL ' + name + (extra ? '\n       ' + extra : '')); }
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function post(p, body) {
  const r = await fetch(BASE + p, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  });
  return { status: r.status, json: await r.json().catch(() => null) };
}
async function get(p) {
  const r = await fetch(BASE + p);
  const text = await r.text();
  let json = null;
  try { json = JSON.parse(text); } catch (e) {}
  return { status: r.status, json, text };
}

(async function main() {
  // ---- the mixer's side of the UDP link -----------------------------------
  const received = [];
  const sock = dgram.createSocket('udp4');
  sock.on('message', (b) => received.push(b));
  await new Promise((r) => sock.bind(MIXER, '127.0.0.1', r));

  const control = dgram.createSocket('udp4');
  await new Promise((r) => control.bind(0, '127.0.0.1', r));
  let controlSeq = 0;
  let heartbeat = { holdArmed: false, windowOpen: true };
  const sendControl = (c) => new Promise((r) => {
    if (c) heartbeat = c;
    control.send(pkt.encodeControl(Object.assign({ seq: ++controlSeq }, heartbeat)),
                 CONTROL, '127.0.0.1', r);
  });
  // Stand in for a running mixer, which sends its controls at 10 Hz.
  const hbTimer = setInterval(() => { sendControl(null); }, 100);

  // ---- start the server ---------------------------------------------------
  const srv = spawn(process.execPath, [path.join(__dirname, '..', 'server.js')], {
    env: Object.assign({}, process.env, {
      CROWD_HTTP_PORT: String(HTTP),
      CROWD_MIXER_PORT: String(MIXER),
      CROWD_CONTROL_PORT: String(CONTROL),
      CROWD_VJ_TOKEN: TOKEN,
    }),
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  let serverLog = '';
  srv.stdout.on('data', (d) => { serverLog += d; });
  srv.stderr.on('data', (d) => { serverLog += d; });
  const stop = () => { try { srv.kill(); } catch (e) {} };
  process.on('exit', stop);

  await sleep(900);
  await sleep(400);   // let a few heartbeats land so the window is open

  try {
    const hOnline = await get('/health');
    check('the server sees the mixer once controls arrive',
      hOnline.json.mixerOnline === true && hOnline.json.state.windowOpen === true);

    // ---- basic liveness ---------------------------------------------------
    const health = await get('/health');
    check('server answers /health', health.status === 200 && health.json && health.json.ok);

    const page = await get('/');
    check('audience page is served', page.status === 200 && /TAP/.test(page.text));

    // ---- UDP to the mixer -------------------------------------------------
    await sleep(400);
    check('UDP state packets arrive', received.length >= 4,
      'got ' + received.length + ' in ~1.3 s at 20 Hz');
    const b = received[received.length - 1];
    check('packet is the 20-byte VJCR format',
      b.length === 20 && b.slice(0, 4).toString('ascii') === 'VJCR' &&
      b.readUInt8(4) === pkt.VERSION);
    const seqs = received.slice(-10).map((x) => x.readUInt16LE(6));
    check('sequence advances', seqs.every((v, i) => i === 0 || pkt.seqNewer(v, seqs[i - 1])),
      seqs.join(','));

    // ---- the VJ page is not open to the room ------------------------------
    const noTok = await get('/vj');
    check('/vj refuses a missing token', noTok.status === 403);
    const badTok = await get('/vj?t=nope');
    check('/vj refuses a wrong token', badTok.status === 403);
    const okTok = await get('/vj?t=' + TOKEN);
    check('/vj opens with the right token from loopback', okTok.status === 200);

    // ---- taps -------------------------------------------------------------
    const bad = await post('/tap', { taps: 5 });          // no id
    check('/tap rejects a body with no device id', bad.status === 400);

    // 6 phones, 250 ms batches, each claiming 80 taps/s — sixteen times what
    // the bucket allows. What matters is the rate the server credits, not the
    // ratio (the ratio just reflects how absurd the claim was).
    const NPHONE = 6, CLAIM = 20, ROUNDS = 24;
    let claimed = 0, accepted = 0;
    const t0 = Date.now();
    for (let round = 0; round < ROUNDS; round++) {
      const rs = await Promise.all(
        Array.from({ length: NPHONE }, (_, i) =>
          post('/tap', { id: 'phone' + i, taps: CLAIM })));
      for (const r of rs) { claimed += CLAIM; accepted += r.json.accepted; }
      await sleep(250);
    }
    const elapsed = (Date.now() - t0) / 1000;
    const perDevice = accepted / NPHONE / elapsed;
    check('the server credits far fewer taps than were claimed',
      accepted < claimed * 0.2, accepted + ' of ' + claimed);
    check('each device is credited at about the refill rate, not its claim',
      perDevice > 3.0 && perDevice < 5.5,
      perDevice.toFixed(2) + ' taps/s/device (bucket refills at 5)');

    const h2 = await get('/health');
    check('server-side totals match what the clients were told',
      h2.json.stats.accepted === accepted,
      'server ' + h2.json.stats.accepted + ' vs clients ' + accepted);

    // 6 phones at the default tuning need ~15 s to fill; rather than sit here
    // for that long, raise the gain through the real /vj endpoint (which also
    // exercises live tuning) and keep tapping until it goes off.
    const tune = await post('/vj/params?t=' + TOKEN, { baseGain: 0.08 });
    check('live tuning through /vj is applied', tune.status === 200 &&
      Math.abs(tune.json.cfg.baseGain - 0.08) < 1e-9);

    let bursts = 0;
    for (let round = 0; round < 40 && bursts === 0; round++) {
      await Promise.all(Array.from({ length: NPHONE }, (_, i) =>
        post('/tap', { id: 'phone' + i, taps: CLAIM })));
      await sleep(250);
      bursts = (await get('/health')).json.state.bursts;
    }
    check('a burst fired from real HTTP traffic', bursts >= 1, 'bursts=' + bursts);

    // A burst must show up on the wire, not just in /health.
    const sawBurst = received.some((x) => x.readFloatLE(12) > 0.5);
    check('the burst reached the mixer over UDP', sawBurst);

    // ---- control back-channel --------------------------------------------
    await sendControl({ holdArmed: true, windowOpen: true });
    await sleep(200);
    const h3 = await get('/health');
    check('mixer control packet engages HOLD', h3.json.state.holdArmed === true);

    await sendControl({ holdArmed: false, windowOpen: false });
    await sleep(200);
    await post('/vj/params?t=' + TOKEN, { baseGain: 0.011 });   // back to default
    await sleep(400);
    const before = (await get('/health')).json.state.charge;
    for (let i = 0; i < 6; i++) {
      await Promise.all(Array.from({ length: 6 }, (_, k) =>
        post('/tap', { id: 'phone' + k, taps: 20 })));
      await sleep(120);
    }
    const h4 = await get('/health');
    check('a closed window stops the gauge moving',
      h4.json.state.windowOpen === false && h4.json.state.charge <= before + 1e-6,
      'charge ' + before.toFixed(3) + ' -> ' + h4.json.state.charge.toFixed(3));

    // ---- junk on the control socket ---------------------------------------
    await new Promise((r) => control.send(Buffer.from('garbage!'), CONTROL, '127.0.0.1', r));
    await new Promise((r) => control.send(Buffer.alloc(400), CONTROL, '127.0.0.1', r));
    const stale = pkt.encodeControl({ holdArmed: true, windowOpen: true, seq: 1 });
    await new Promise((r) => control.send(stale, CONTROL, '127.0.0.1', r));  // old seq
    await sleep(300);
    const h5 = await get('/health');
    check('junk and out-of-order control packets are ignored, server alive',
      h5.status === 200 && h5.json.state.holdArmed === false &&
      h5.json.state.windowOpen === false);

    // ---- reopening recovers ------------------------------------------------
    await sendControl({ holdArmed: false, windowOpen: true });
    await sleep(200);
    const h6 = await get('/health');
    check('reopening the window works', h6.json.state.windowOpen === true);

    // ---- the mixer going away ---------------------------------------------
    clearInterval(hbTimer);
    await sleep(3500);
    const hGone = await get('/health');
    check('losing the mixer closes the window instead of hanging on',
      hGone.json.mixerOnline === false && hGone.json.state.windowOpen === false);

    // ---- a restarted mixer starts its sequence at 0 again ------------------
    // This is the one that used to lock the link out for the best part of an
    // hour: the old sequence check rejected everything below what it had seen.
    controlSeq = 0;
    await sendControl({ holdArmed: false, windowOpen: true });
    await sleep(300);
    await sendControl(null);
    await sleep(300);
    const hBack = await get('/health');
    check('a restarted mixer (seq back to 0) is picked up again',
      hBack.json.mixerOnline === true && hBack.json.state.windowOpen === true);

    // ---- SSE connections are capped ---------------------------------------
    const streams = [];
    let refused = 0;
    for (let i = 0; i < 6; i++) {
      const ac = new AbortController();
      streams.push(ac);
      try {
        const r = await fetch(BASE + '/events', { signal: ac.signal });
        if (r.status === 503) refused++;
        else r.body.getReader();   // hold it open
      } catch (e) { /* aborted */ }
    }
    check('too many event streams from one address are refused', refused > 0,
      'refused ' + refused + ' of 6');
    streams.forEach((a) => { try { a.abort(); } catch (e) {} });

  } catch (e) {
    failed++;
    console.log('  FAIL threw: ' + e.stack);
  } finally {
    clearInterval(hbTimer);
    stop();
    sock.close();
    control.close();
  }

  console.log('');
  console.log(passed + ' passed, ' + failed + ' failed');
  if (failed) {
    console.log('--- server output ---\n' + serverLog);
    process.exitCode = 1;
  }
  process.exit(failed ? 1 : 0);
})();
