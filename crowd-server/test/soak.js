'use strict';
// Soak test: keep 20 simulated phones tapping for a while and watch for the
// things a 60-second test cannot see — memory creeping up, the UDP cadence
// drifting, device maps growing without bound.
//
//   node crowd-server/test/soak.js [minutes]

const { spawn } = require('child_process');
const dgram = require('dgram');
const path = require('path');
const pkt = require('../packet.js');

const MINUTES = parseFloat(process.argv[2] || '12');
const HTTP = 8887, MIXER = 8888, CONTROL = 8889;
const BASE = 'http://127.0.0.1:' + HTTP;
const PHONES = 20;
const TOKEN = 'soak';

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

(async function main() {
  let packets = 0;
  let lastSeq = -1, seqGaps = 0, seqBackwards = 0;
  const sock = dgram.createSocket('udp4');
  sock.on('message', (b) => {
    packets++;
    if (b.length !== 20) return;
    const s = b.readUInt16LE(6);
    if (lastSeq >= 0) {
      const d = ((s - lastSeq) << 16) >> 16;
      if (d <= 0) seqBackwards++;
      else if (d > 1) seqGaps++;
    }
    lastSeq = s;
  });
  await new Promise((r) => sock.bind(MIXER, '127.0.0.1', r));

  const srv = spawn(process.execPath, [path.join(__dirname, '..', 'server.js')], {
    env: Object.assign({}, process.env, {
      CROWD_HTTP_PORT: String(HTTP),
      CROWD_MIXER_PORT: String(MIXER),
      CROWD_CONTROL_PORT: String(CONTROL),
      CROWD_VJ_TOKEN: TOKEN,
    }),
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  let bursts = 0;
  srv.stdout.on('data', (d) => { if (/BURST/.test(String(d))) bursts++; });
  process.on('exit', () => { try { srv.kill(); } catch (e) {} });

  await sleep(1000);

  let stop = false;
  let posts = 0, postErrors = 0;
  // Each phone POSTs its batch every 250 ms, exactly like the real page.
  const phone = async (i) => {
    while (!stop) {
      try {
        const r = await fetch(BASE + '/tap', {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ id: 'soak' + i, taps: 2 }),
        });
        await r.json();
        posts++;
      } catch (e) { postErrors++; }
      await sleep(250);
    }
  };
  const runners = Array.from({ length: PHONES }, (_, i) => phone(i));

  const t0 = Date.now();
  const samples = [];
  const endAt = t0 + MINUTES * 60000;
  let lastPackets = 0, lastSampleAt = Date.now();

  console.log('soak: ' + PHONES + ' phones, ' + MINUTES + ' min, sampling every 60 s');
  console.log('  min   rssMB   pkt/s   devices  charge  bursts  gaps  back  postErr');

  while (Date.now() < endAt) {
    await sleep(60000);
    const now = Date.now();
    let h = null;
    try { h = await (await fetch(BASE + '/health')).json(); } catch (e) {}
    let rss = 0;
    try {
      // Windows: ask tasklist for the child's working set.
      rss = await new Promise((res) => {
        const t = spawn('tasklist', ['/FI', 'PID eq ' + srv.pid, '/FO', 'CSV', '/NH']);
        let out = '';
        t.stdout.on('data', (d) => { out += d; });
        t.on('close', () => {
          const m = out.match(/"([\d,]+) K"/);
          res(m ? Math.round(parseInt(m[1].replace(/,/g, ''), 10) / 1024) : 0);
        });
        t.on('error', () => res(0));
      });
    } catch (e) {}
    const dtSec = (now - lastSampleAt) / 1000;
    const rate = (packets - lastPackets) / dtSec;
    lastPackets = packets; lastSampleAt = now;
    const row = {
      min: ((now - t0) / 60000).toFixed(1),
      rss, rate: rate.toFixed(1),
      devices: h ? h.state.devices : -1,
      charge: h ? h.state.charge.toFixed(2) : '?',
      bursts, seqGaps, seqBackwards, postErrors,
    };
    samples.push(row);
    console.log('  ' + row.min.padStart(4) + '  ' + String(rss).padStart(6) +
                '  ' + row.rate.padStart(6) + '  ' + String(row.devices).padStart(7) +
                '  ' + String(row.charge).padStart(6) + '  ' + String(bursts).padStart(6) +
                '  ' + String(seqGaps).padStart(4) + '  ' + String(seqBackwards).padStart(4) +
                '  ' + String(postErrors).padStart(7));
  }

  stop = true;
  await Promise.all(runners).catch(() => {});
  await sleep(300);

  // Verdict.
  let bad = 0;
  const first = samples[0], last = samples[samples.length - 1];
  console.log('');
  if (samples.length >= 2 && first.rss > 0) {
    const growth = last.rss - first.rss;
    console.log('  memory ' + first.rss + ' -> ' + last.rss + ' MB (' +
                (growth >= 0 ? '+' : '') + growth + ')');
    if (growth > 40) { console.log('  FAIL memory grew more than 40 MB'); bad++; }
  }
  const rates = samples.map((x) => parseFloat(x.rate));
  const minRate = Math.min.apply(null, rates), maxRate = Math.max.apply(null, rates);
  // Nominal is 20 Hz but Windows timer granularity (~15.6 ms) puts the real
  // rate near 16. What matters is that it is steady and stays well inside the
  // mixer's 400 ms hold, not that it hits the nominal number.
  console.log('  UDP cadence ' + minRate.toFixed(1) + '-' + maxRate.toFixed(1) +
              ' pkt/s (nominal 20, ~16 expected on Windows)');
  if (minRate < 12) { console.log('  FAIL cadence too slow'); bad++; }
  if (maxRate - minRate > 4) { console.log('  FAIL cadence unstable'); bad++; }
  console.log('  device map: ' + last.devices + ' (expected ' + PHONES + ')');
  if (last.devices > PHONES + 2) { console.log('  FAIL device map grew'); bad++; }
  console.log('  bursts ' + bursts + ', seq gaps ' + seqGaps +
              ', backwards ' + seqBackwards + ', POST errors ' + postErrors);
  if (seqBackwards > 0) { console.log('  FAIL sequence went backwards'); bad++; }
  if (postErrors > posts * 0.01) { console.log('  FAIL too many POST failures'); bad++; }

  console.log('');
  console.log(bad === 0 ? 'soak OK' : 'soak FAILED (' + bad + ')');
  try { srv.kill(); } catch (e) {}
  sock.close();
  process.exit(bad === 0 ? 0 : 1);
})();
