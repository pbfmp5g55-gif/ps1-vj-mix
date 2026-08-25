'use strict';
// CROWD gauge — the whole "feel" of the audience feature lives here.
// Pure state machine, no I/O, so test/selftest.js can drive it directly.
//
// Design: ../design/CROWD_CONTROL.md

const DEFAULTS = {
  // Tunable live from /vj.
  // Defaults tuned so the fill time spans a usable range across the crowd
  // sizes this is built for (see the table in crowd-server/README.md):
  // 3 people flat out ~20 s, 10 people ~8 s, 20 people ~5 s, and an
  // abandoned gauge decays away in ~25 s. Live-tunable from /vj.
  baseGain:      0.011,  // charge added per accepted tap, before crowd scaling
  leakRate:      0.04,   // charge drained per second
  burstDecaySec: 2.5,    // seconds for a burst to fall from 1 to 0
  cooldownSec:   5.0,    // no second burst inside this window

  // Fixed for the MVP.
  bucketCapacity:   5,   // per-device burst allowance
  bucketRefillPerSec: 5, // per-device sustained taps/s
  activeWindowSec: 10,   // "active" = tapped at least once within this
  activeSmoothTau:  7,   // seconds; smooths the divisor so it can't jump
  activeFloor:      3,   // never divide by less than this
  quorum:           2,   // devices that must be active for a burst to fire
  cooldownChargeCeil: 0.3, // charge is capped here during cooldown
  // Anti-flood: distinct device ids accepted from one address. Phones on a
  // venue LAN each have their own IP, so this only ever bites a machine
  // spinning up fake ids — or a test driving everything from loopback, which
  // is how the first value of 4 was caught silently swallowing 16 of 20
  // simulated phones with no feedback at all.
  maxDevicesPerAddr: 12,
};

// Per-device token bucket. Refilled from elapsed time on the server, so a
// client that lies about how many taps it sent gains nothing.
class Bucket {
  constructor(cfg, nowSec, addr) {
    this.tokens = cfg.bucketCapacity;
    this.last = nowSec;
    this.lastTapAt = -Infinity;
    this.accepted = 0;
    this.rejected = 0;
    this.addr = addr;   // kept so prune() can free the per-address slot too
  }
  take(cfg, claimed, nowSec) {
    const dt = Math.max(0, nowSec - this.last);
    this.last = nowSec;
    this.tokens = Math.min(cfg.bucketCapacity,
                           this.tokens + dt * cfg.bucketRefillPerSec);
    // A client can only ever spend tokens the server itself has refilled.
    // Normalise first: a fractional or absurd claim would otherwise flow
    // straight into the running totals (Infinity poisons every later stat).
    const asInt = Math.floor(Number(claimed));
    const want = Number.isFinite(asInt)
      ? Math.max(0, Math.min(asInt, cfg.bucketCapacity))
      : 0;
    const got = Math.min(want, Math.floor(this.tokens));
    this.tokens -= got;
    this.accepted += got;
    this.rejected += Math.max(0, want - got);
    if (got > 0) this.lastTapAt = nowSec;
    return got;
  }
}

class Gauge {
  constructor(cfg = {}) {
    this.cfg = Object.assign({}, DEFAULTS, cfg);
    this.charge = 0;
    this.burst = 0;
    this.cooldownRemaining = 0;
    this.devices = new Map();      // deviceId -> Bucket
    this.addrDevices = new Map();  // remoteAddr -> Set(deviceId)
    this.smoothedActive = 0;
    this.pendingTaps = 0;          // accepted taps not yet folded into charge
    this.seq = 0;
    // Latched when the gauge fills with a real crowd behind it. Without it,
    // a room that fills the gauge and then stops tapping falls under quorum
    // within activeWindowSec, and releasing HOLD would fire nothing.
    this.quorumLatched = false;
    // Performance controls owned by the mixer (arrive over the back-channel).
    this.holdArmed = false;        // true = do not fire, wait for the VJ
    this.windowOpen = true;        // false = participation window is closed
    this.totalBursts = 0;
  }

  // Bounds, not just type checks: burstDecaySec = 0 used to leave a fired
  // burst pinned at 1.0 for the rest of the night.
  static get LIMITS() {
    return {
      baseGain:      [0.0005, 1.0],
      leakRate:      [0.0,    5.0],
      burstDecaySec: [0.05,  60.0],
      cooldownSec:   [0.0,  600.0],
    };
  }

  setParams(patch) {
    const lim = Gauge.LIMITS;
    for (const k of Object.keys(lim)) {
      const v = Number(patch[k]);
      if (!Number.isFinite(v)) continue;
      this.cfg[k] = Math.min(lim[k][1], Math.max(lim[k][0], v));
    }
  }

  // True when a *new* device from this address would be turned away. The
  // server asks first so the phone can be told, instead of tapping into a
  // void and concluding the whole thing is broken.
  deviceBlocked(deviceId, remoteAddr) {
    if (this.devices.has(deviceId)) return false;
    const ids = this.addrDevices.get(remoteAddr);
    return !!ids && ids.size >= this.cfg.maxDevicesPerAddr;
  }

  // Returns the number of taps actually credited.
  tap(deviceId, claimedTaps, nowSec, remoteAddr = '?') {
    if (!this.windowOpen) return 0;
    const n = Math.floor(Number(claimedTaps));
    if (!Number.isFinite(n) || n <= 0) return 0;

    let bucket = this.devices.get(deviceId);
    if (!bucket) {
      let ids = this.addrDevices.get(remoteAddr);
      if (!ids) { ids = new Set(); this.addrDevices.set(remoteAddr, ids); }
      if (ids.size >= this.cfg.maxDevicesPerAddr) return 0;
      ids.add(deviceId);
      bucket = new Bucket(this.cfg, nowSec, remoteAddr);
      this.devices.set(deviceId, bucket);
    }
    const got = bucket.take(this.cfg, n, nowSec);
    this.pendingTaps += got;
    return got;
  }

  activeCount(nowSec) {
    let n = 0;
    for (const b of this.devices.values()) {
      if (nowSec - b.lastTapAt <= this.cfg.activeWindowSec) ++n;
    }
    return n;
  }

  // dt in seconds. Call at a steady rate (the server ticks at 20 Hz).
  tick(dt, nowSec) {
    const c = this.cfg;
    if (!(dt > 0) || !isFinite(dt)) dt = 0;
    dt = Math.min(dt, 0.25);  // a stalled event loop must not dump charge in

    const rawActive = this.activeCount(nowSec);
    // Exponential smoothing so the divisor cannot jump when one person
    // leaves (which would otherwise make the gauge lurch).
    const alpha = c.activeSmoothTau > 0
      ? 1 - Math.exp(-dt / c.activeSmoothTau)
      : 1;
    this.smoothedActive += (rawActive - this.smoothedActive) * alpha;

    // sqrt, not linear: total fill rate then grows with sqrt(N), so a bigger
    // crowd is genuinely faster without 20 people filling it instantly.
    // Dividing by N (the first draft) made fill time independent of N.
    const divisor = Math.sqrt(Math.max(c.activeFloor, this.smoothedActive));
    const gainPerTap = c.baseGain / divisor;

    this.charge += this.pendingTaps * gainPerTap;
    this.pendingTaps = 0;
    // A gauge that is full and being held is *loaded*: it must not leak away
    // while the VJ waits for the right bar. Any other time it drains.
    const loaded = this.holdArmed && this.charge >= 1.0;
    if (!loaded) this.charge -= c.leakRate * dt;

    if (this.cooldownRemaining > 0) this.cooldownRemaining -= dt;
    const inCooldown = this.cooldownRemaining > 0;
    const ceil = inCooldown ? c.cooldownChargeCeil : 1.0;
    this.charge = Math.max(0, Math.min(ceil, this.charge));

    // Latch "full, with a real crowd behind it" rather than re-testing
    // charge >= 1.0 at the moment of firing: on the tick the VJ releases HOLD
    // the leak has already taken charge a hair under 1.0, and the drop would
    // never land. The latch clears if the gauge actually drains away.
    if (this.charge >= 1.0 && rawActive >= c.quorum) this.quorumLatched = true;
    if (this.charge < 0.95) this.quorumLatched = false;

    const canFire = !inCooldown && this.quorumLatched &&
                    !this.holdArmed && this.windowOpen;
    let fired = false;
    if (canFire) {
      this.burst = 1.0;
      this.charge = 0;
      this.cooldownRemaining = c.cooldownSec;
      this.totalBursts++;
      this.quorumLatched = false;
      fired = true;
    }

    if (this.burst > 0) {
      this.burst = c.burstDecaySec > 0
        ? Math.max(0, this.burst - dt / c.burstDecaySec)
        : 0;
    }

    this.seq = (this.seq + 1) & 0xffff;
    return { fired, rawActive };
  }

  // Charge is "full and waiting" when HOLD is on. The VJ releasing HOLD
  // fires it on the next tick.
  get held() {
    return this.holdArmed && this.charge >= 1.0;
  }

  state(nowSec) {
    return {
      charge: this.charge,
      burst: this.burst,
      cooldown: Math.max(0, this.cooldownRemaining),
      active: this.activeCount(nowSec),
      devices: this.devices.size,
      held: this.held,
      holdArmed: this.holdArmed,
      windowOpen: this.windowOpen,
      bursts: this.totalBursts,
      seq: this.seq,
    };
  }

  // Drop devices that have not tapped for a long time so the maps do not
  // grow without bound across a long set.
  // Dropping a device from `devices` without also freeing its slot in
  // `addrDevices` meant an address that ever reached the cap could never
  // register another device again, for the life of the process.
  prune(nowSec, idleSec = 300) {
    for (const [id, b] of this.devices) {
      if (nowSec - b.lastTapAt <= idleSec) continue;
      this.devices.delete(id);
      const ids = this.addrDevices.get(b.addr);
      if (ids) {
        ids.delete(id);
        if (ids.size === 0) this.addrDevices.delete(b.addr);
      }
    }
  }
}

module.exports = { Gauge, DEFAULTS };
