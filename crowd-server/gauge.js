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
  maxDevicesPerAddr:  4,   // crude anti-flood: distinct device ids per IP
};

// Per-device token bucket. Refilled from elapsed time on the server, so a
// client that lies about how many taps it sent gains nothing.
class Bucket {
  constructor(cfg, nowSec) {
    this.tokens = cfg.bucketCapacity;
    this.last = nowSec;
    this.lastTapAt = -Infinity;
    this.accepted = 0;
    this.rejected = 0;
  }
  take(cfg, claimed, nowSec) {
    const dt = Math.max(0, nowSec - this.last);
    this.last = nowSec;
    this.tokens = Math.min(cfg.bucketCapacity,
                           this.tokens + dt * cfg.bucketRefillPerSec);
    // A client can only ever spend tokens the server itself has refilled.
    const want = Math.max(0, Math.min(claimed, cfg.bucketCapacity));
    const got = Math.min(want, Math.floor(this.tokens));
    this.tokens -= got;
    this.accepted += got;
    this.rejected += Math.max(0, claimed - got);
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
    // Performance controls owned by the mixer (arrive over the back-channel).
    this.holdArmed = false;        // true = do not fire, wait for the VJ
    this.windowOpen = true;        // false = participation window is closed
    this.totalBursts = 0;
  }

  setParams(patch) {
    for (const k of ['baseGain', 'leakRate', 'burstDecaySec', 'cooldownSec']) {
      if (typeof patch[k] === 'number' && isFinite(patch[k]) && patch[k] >= 0) {
        this.cfg[k] = patch[k];
      }
    }
  }

  // Returns the number of taps actually credited.
  tap(deviceId, claimedTaps, nowSec, remoteAddr = '?') {
    if (!this.windowOpen) return 0;
    const n = Number(claimedTaps);
    if (!isFinite(n) || n <= 0) return 0;

    let bucket = this.devices.get(deviceId);
    if (!bucket) {
      let ids = this.addrDevices.get(remoteAddr);
      if (!ids) { ids = new Set(); this.addrDevices.set(remoteAddr, ids); }
      if (ids.size >= this.cfg.maxDevicesPerAddr) return 0;
      ids.add(deviceId);
      bucket = new Bucket(this.cfg, nowSec);
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
    this.charge -= c.leakRate * dt;

    if (this.cooldownRemaining > 0) this.cooldownRemaining -= dt;
    const inCooldown = this.cooldownRemaining > 0;
    const ceil = inCooldown ? c.cooldownChargeCeil : 1.0;
    this.charge = Math.max(0, Math.min(ceil, this.charge));

    const canFire = !inCooldown &&
                    rawActive >= c.quorum &&
                    !this.holdArmed &&
                    this.windowOpen;
    let fired = false;
    if (this.charge >= 1.0 && canFire) {
      this.burst = 1.0;
      this.charge = 0;
      this.cooldownRemaining = c.cooldownSec;
      this.totalBursts++;
      fired = true;
    }

    if (this.burst > 0 && c.burstDecaySec > 0) {
      this.burst = Math.max(0, this.burst - dt / c.burstDecaySec);
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
  prune(nowSec, idleSec = 300) {
    for (const [id, b] of this.devices) {
      if (nowSec - b.lastTapAt > idleSec) this.devices.delete(id);
    }
    if (this.devices.size === 0) this.addrDevices.clear();
  }
}

module.exports = { Gauge, DEFAULTS };
