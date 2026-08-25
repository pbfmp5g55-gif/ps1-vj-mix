'use strict';
// Gauge + wire-format selftest. Pure logic, fake clock, no sockets — so the
// numbers below are reproducible and a failure points at the maths, not at
// the network.
//
//   node crowd-server/test/selftest.js

const assert = require('assert');
const { Gauge } = require('../gauge.js');
const pkt = require('../packet.js');

let passed = 0;
function test(name, fn) {
  try { fn(); passed++; console.log('  ok   ' + name); }
  catch (e) { console.log('  FAIL ' + name + '\n       ' + e.message); process.exitCode = 1; }
}

// A gauge plus a monotonic fake clock. Every step is 1/20 s, the same rate
// the server ticks at. The clock never jumps, because jumping it silently
// expires token buckets and the active window and makes tests lie.
class Sim {
  constructor(cfg) {
    this.g = new Gauge(cfg);
    this.t = 1000;
    this.log = [];
    this.carry = 0;   // fractional taps, kept across calls so run() can be
                      // driven one tick at a time without losing every tap
  }
  // Run `seconds` with `tappers` devices each claiming `tapsPerSec`.
  run(seconds, tappers, tapsPerSec) {
    const HZ = 20, dt = 1 / HZ;
    const perTick = tapsPerSec / HZ;
    const from = this.log.length;
    for (let i = 0; i < Math.round(seconds * HZ); i++) {
      this.carry += perTick;
      const n = Math.floor(this.carry);
      this.carry -= n;
      if (n > 0) for (let d = 0; d < tappers; d++) this.g.tap('dev' + d, n, this.t);
      const r = this.g.tick(dt, this.t);
      this.log.push({ t: this.t - 1000, charge: this.g.charge, burst: this.g.burst,
                      cooldown: this.g.cooldownRemaining, fired: r.fired });
      this.t += dt;
    }
    return this.log.slice(from);
  }
  // Run until the tick that fires a burst, and stop right there, so a test
  // that cares about the cooldown starts from a known instant.
  runUntilBurst(maxSeconds, tappers, tapsPerSec) {
    const HZ = 20;
    for (let i = 0; i < Math.round(maxSeconds * HZ); i++) {
      const [e] = this.run(1 / HZ, tappers, tapsPerSec);
      if (e && e.fired) return true;
    }
    return false;
  }
  timeToFirstBurst() {
    const hit = this.log.find((e) => e.fired);
    return hit ? hit.t : Infinity;
  }
}

console.log('gauge');

test('leaks to zero when nobody taps', () => {
  const s = new Sim();
  s.g.charge = 1.0;
  s.run(40, 0, 0);
  assert.strictEqual(s.g.charge, 0, 'charge should drain to exactly 0');
});

test('token bucket caps a device that claims absurd numbers', () => {
  const g = new Gauge();
  const cap = g.cfg.bucketCapacity;
  assert.strictEqual(g.tap('a', 10000, 1000), cap, 'one shot exceeded the bucket');
  let total = 0;
  for (let i = 1; i <= 200; i++) total += g.tap('a', 10000, 1000 + i * 0.05);
  const expected = 10 * g.cfg.bucketRefillPerSec;
  assert.ok(Math.abs(total - expected) <= 1,
    'sustained accept ' + total + ' should be ~' + expected);
});

test('one person flat out cannot even hold the gauge up', () => {
  // Deliberate at the default tuning: a solo tapper earns less per second
  // than the leak drains, so the gauge is a group effort by arithmetic and
  // not only by the quorum rule.
  const s = new Sim();
  s.run(60, 1, 20);
  assert.ok(s.g.charge < 0.05, 'solo tapper reached ' + s.g.charge.toFixed(3));
});

test('quorum blocks a solo fill even when the gain is cranked right up', () => {
  const s = new Sim({ baseGain: 0.5 });   // one person would fill in ~1 s
  s.run(60, 1, 20);
  assert.ok(!s.log.some((e) => e.fired), 'a single device fired a burst');
  // It hovers just under 1.0 rather than sitting exactly on it: the leak is
  // subtracted every tick, and the token bucket only lets a tap through on
  // some of them. Full-and-waiting is the claim, not an exact value.
  assert.ok(s.g.charge >= 0.95, 'charge should sit pinned near full, was ' + s.g.charge);
});

test('two tappers do fire', () => {
  const s = new Sim();
  s.run(180, 2, 20);
  assert.ok(s.log.some((e) => e.fired), 'two devices never fired');
});

test('a bigger crowd fills faster (sqrt, not flat)', () => {
  const a = new Sim(); a.run(180, 3, 5);
  const b = new Sim(); b.run(180, 20, 5);
  const t3 = a.timeToFirstBurst(), t20 = b.timeToFirstBurst();
  assert.ok(isFinite(t3) && isFinite(t20), 'both crowds should reach a burst');
  assert.ok(t20 < t3 * 0.8,
    '20 people (' + t20.toFixed(1) + 's) must beat 3 people (' + t3.toFixed(1) + 's); ' +
    'dividing by N instead of sqrt(N) would make these equal');
});

test('cooldown does not auto-refire once everyone stops', () => {
  const s = new Sim();
  assert.ok(s.runUntilBurst(120, 5, 10), 'setup failed to produce a burst');
  const before = s.g.totalBursts;
  s.run(s.g.cfg.cooldownSec + 5, 0, 0);   // everyone stops the moment it fires
  assert.strictEqual(s.g.totalBursts, before,
    'refired with nobody tapping — charge kept accumulating through cooldown');
});

test('charge stays under the ceiling while the cooldown runs', () => {
  const s = new Sim();
  assert.ok(s.runUntilBurst(120, 10, 10), 'setup failed to produce a burst');
  const during = s.run(2, 10, 20).filter((e) => e.cooldown > 0);
  assert.ok(during.length > 0, 'expected to still be inside the cooldown');
  assert.ok(during.every((e) => e.charge <= s.g.cfg.cooldownChargeCeil + 1e-6),
    'charge exceeded the cooldown ceiling');
});

test('HOLD blocks the burst, and releasing it fires', () => {
  const s = new Sim();
  s.g.holdArmed = true;
  s.run(60, 5, 10);
  assert.ok(!s.log.some((e) => e.fired), 'fired while HOLD was armed');
  assert.ok(s.g.held, 'should report held (full and waiting)');
  s.g.holdArmed = false;
  const after = s.run(0.5, 5, 10);
  assert.ok(after.some((e) => e.fired), 'releasing HOLD did not fire');
});

test('a closed window rejects taps outright', () => {
  const s = new Sim();
  s.g.windowOpen = false;
  assert.strictEqual(s.g.tap('a', 5, 1000), 0);
  s.run(20, 10, 10);
  assert.strictEqual(s.g.charge, 0);
});

test('a stalled event loop cannot dump charge in one tick', () => {
  const g = new Gauge();
  g.charge = 1.0;
  g.tick(600, 1000);   // pretend the process was frozen for 10 minutes
  assert.ok(g.charge > 0.9, 'a huge dt drained the gauge instantly');
});

test('one IP cannot farm unlimited device ids', () => {
  const g = new Gauge();
  const max = g.cfg.maxDevicesPerAddr;
  let allowed = 0;
  for (let i = 0; i < max + 10; i++) {
    if (g.tap('id' + i, 1, 1000, '192.168.1.50') > 0) allowed++;
  }
  assert.strictEqual(allowed, max);
});

test('active count decays out of the window', () => {
  const g = new Gauge();
  g.tap('a', 3, 1000);
  assert.strictEqual(g.activeCount(1000), 1);
  assert.strictEqual(g.activeCount(1000 + g.cfg.activeWindowSec + 1), 0);
});

test('one person leaving does not lurch the divisor', () => {
  // The divisor is smoothed, so losing a tapper must not hand the rest a
  // sudden speed-up on the very next tick.
  const s = new Sim();
  s.run(20, 10, 5);
  const before = s.g.smoothedActive;
  s.run(0.1, 5, 5);
  assert.ok(Math.abs(s.g.smoothedActive - before) < 0.5,
    'divisor jumped by ' + Math.abs(s.g.smoothedActive - before).toFixed(2));
});

console.log('wire format');

test('state packet is 20 bytes with the expected layout', () => {
  const b = pkt.encodeState({
    charge: 0.25, burst: 0.5, active: 7, cooldown: 1.5, seq: 4242,
    held: true, holdArmed: true, windowOpen: true,
  });
  assert.strictEqual(b.length, 20);
  assert.strictEqual(b.slice(0, 4).toString('ascii'), 'VJCR');
  assert.strictEqual(b.readUInt8(4), pkt.VERSION);
  assert.strictEqual(b.readUInt16LE(6), 4242);
  assert.ok(Math.abs(b.readFloatLE(8) - 0.25) < 1e-6);
  assert.ok(Math.abs(b.readFloatLE(12) - 0.5) < 1e-6);
  assert.strictEqual(b.readUInt16LE(16), 7);
  assert.strictEqual(b.readUInt16LE(18), 150);   // centiseconds
  const f = b.readUInt8(5);
  assert.ok(f & pkt.FLAG_HELD && f & pkt.FLAG_HOLD_ARMED && f & pkt.FLAG_WINDOW_OPEN);
  assert.ok(f & pkt.FLAG_COOLDOWN);
});

test('NaN and out-of-range values are clamped, never emitted raw', () => {
  const b = pkt.encodeState({
    charge: NaN, burst: 99, active: -5, cooldown: -1, seq: 70000,
    held: false, holdArmed: false, windowOpen: false,
  });
  assert.strictEqual(b.readFloatLE(8), 0);
  assert.strictEqual(b.readFloatLE(12), 1);
  assert.strictEqual(b.readUInt16LE(18), 0);
});

test('control packets round-trip and junk is rejected', () => {
  const c = pkt.decodeControl(pkt.encodeControl({ holdArmed: true, windowOpen: false, seq: 9 }));
  assert.deepStrictEqual(c, { holdArmed: true, windowOpen: false, seq: 9 });
  assert.strictEqual(pkt.decodeControl(Buffer.alloc(8)), null, 'bad magic accepted');
  assert.strictEqual(pkt.decodeControl(Buffer.alloc(7)), null, 'short packet accepted');
  assert.strictEqual(pkt.decodeControl(Buffer.alloc(64)), null, 'long packet accepted');
  const wrongVer = pkt.encodeControl({ seq: 1 });
  wrongVer.writeUInt8(99, 4);
  assert.strictEqual(pkt.decodeControl(wrongVer), null, 'wrong version accepted');
});

test('sequence comparison survives u16 wraparound', () => {
  assert.ok(pkt.seqNewer(0, 65535));
  assert.ok(pkt.seqNewer(3, 1));
  assert.ok(!pkt.seqNewer(65535, 0));
  assert.ok(!pkt.seqNewer(1, 1));
});

console.log('');
console.log(passed + ' passed' + (process.exitCode ? ', SOME FAILED' : ', all ok'));
