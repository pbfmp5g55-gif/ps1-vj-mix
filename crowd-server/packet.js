'use strict';
// Wire format between the crowd server and the mixer. Both directions are
// fixed-length UDP on the loopback interface. Fields are written out
// explicitly (never a struct memcpy) so the C++ side can't drift.

const STATE_MAGIC   = 0x52434a56; // 'V','J','C','R' at bytes 0..3, read LE
const CONTROL_MAGIC = 0x43434a56; // 'V','J','C','C'
const VERSION = 1;
const STATE_BYTES = 20;
const CONTROL_BYTES = 8;

const FLAG_HELD        = 1 << 0; // full but waiting for the VJ (HOLD)
const FLAG_HOLD_ARMED  = 1 << 1; // HOLD mode is engaged
const FLAG_WINDOW_OPEN = 1 << 2; // participation window is open
const FLAG_COOLDOWN    = 1 << 3; // inside the post-burst cooldown

// server -> mixer
function encodeState(s) {
  const b = Buffer.alloc(STATE_BYTES);
  b.writeUInt32LE(STATE_MAGIC, 0);
  b.writeUInt8(VERSION, 4);
  let flags = 0;
  if (s.held)        flags |= FLAG_HELD;
  if (s.holdArmed)   flags |= FLAG_HOLD_ARMED;
  if (s.windowOpen)  flags |= FLAG_WINDOW_OPEN;
  if (s.cooldown > 0) flags |= FLAG_COOLDOWN;
  b.writeUInt8(flags, 5);
  b.writeUInt16LE(s.seq & 0xffff, 6);
  b.writeFloatLE(clamp01(s.charge), 8);
  b.writeFloatLE(clamp01(s.burst), 12);
  b.writeUInt16LE(Math.min(65535, Math.max(0, s.active | 0)), 16);
  b.writeUInt16LE(Math.min(65535, Math.round(Math.max(0, s.cooldown) * 100)), 18);
  return b;
}

// mixer -> server
function decodeControl(buf) {
  if (!Buffer.isBuffer(buf) || buf.length !== CONTROL_BYTES) return null;
  if (buf.readUInt32LE(0) !== CONTROL_MAGIC) return null;
  if (buf.readUInt8(4) !== VERSION) return null;
  const flags = buf.readUInt8(5);
  return {
    holdArmed:  (flags & FLAG_HOLD_ARMED) !== 0,
    windowOpen: (flags & FLAG_WINDOW_OPEN) !== 0,
    seq: buf.readUInt16LE(6),
  };
}

function encodeControl(c) {
  const b = Buffer.alloc(CONTROL_BYTES);
  b.writeUInt32LE(CONTROL_MAGIC, 0);
  b.writeUInt8(VERSION, 4);
  let flags = 0;
  if (c.holdArmed)  flags |= FLAG_HOLD_ARMED;
  if (c.windowOpen) flags |= FLAG_WINDOW_OPEN;
  b.writeUInt8(flags, 5);
  b.writeUInt16LE(c.seq & 0xffff, 6);
  return b;
}

function clamp01(x) {
  if (!isFinite(x)) return 0;
  return x < 0 ? 0 : x > 1 ? 1 : x;
}

// u16 sequence comparison that survives wraparound: true when `a` is newer.
function seqNewer(a, b) {
  return ((a - b) << 16 >> 16) > 0;
}

module.exports = {
  STATE_MAGIC, CONTROL_MAGIC, VERSION, STATE_BYTES, CONTROL_BYTES,
  FLAG_HELD, FLAG_HOLD_ARMED, FLAG_WINDOW_OPEN, FLAG_COOLDOWN,
  encodeState, decodeControl, encodeControl, seqNewer,
};
