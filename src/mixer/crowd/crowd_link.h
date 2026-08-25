#pragma once
// CROWD link — the mixer's end of the audience feature.
//
// The crowd server (crowd-server/, Node) owns the phones, the gauge maths and
// all of the feel tuning. This side does two small things: read the gauge
// state off a loopback UDP socket, and send the performance controls the VJ
// holds (HOLD, participation window) back the other way.
//
// Deliberately dependency-free and non-blocking: if the server is not
// running, or dies mid-set, the mixer keeps drawing exactly as it would
// without the feature. Design: design/CROWD_CONTROL.md

#include <cstdint>

#include "vj/Params.h"

namespace vjmix {

// Defaults; overridable from the command line.
constexpr uint16_t kCrowdListenPort  = 8778;  // server -> mixer (state)
constexpr uint16_t kCrowdControlPort = 8779;  // mixer -> server (controls)

struct CrowdState {
    bool   socketOpen = false;
    float  charge     = 0.0f;   // 0..1, as last received
    float  burst      = 0.0f;   // 0..1, decays server-side
    float  freshness  = 0.0f;   // 1 while packets flow, ramps to 0 when they stop
    int    active     = 0;      // devices tapping right now
    float  cooldown   = 0.0f;   // seconds until another burst may fire
    bool   held       = false;  // full and waiting for the VJ (HOLD)
    bool   inCooldown = false;
    double lastPacketAt = -1.0;
    uint64_t accepted = 0;      // packets applied
    uint64_t dropped  = 0;      // malformed / stale / wrong version
    uint64_t resyncs  = 0;      // times the sequence was re-based after a gap
    char   error[128] = {0};    // non-empty when the socket could not open

    // What the effects should actually use: the received values faded out by
    // freshness, so a dead server rides down instead of cutting.
    float level() const { return charge * freshness; }
    float hit()   const { return burst  * freshness; }
    bool  linkAlive() const { return freshness > 0.0f; }
};

class CrowdLink {
public:
    CrowdLink() = default;
    ~CrowdLink();
    CrowdLink(const CrowdLink&) = delete;
    CrowdLink& operator=(const CrowdLink&) = delete;

    // Binds the listen socket on loopback. Returns false and fills
    // state().error on failure (port already taken is the usual reason).
    bool open(uint16_t listenPort = kCrowdListenPort,
              uint16_t controlPort = kCrowdControlPort);
    void close();

    // Drain everything queued, keep the newest packet by sequence number.
    // Cheap and never blocks; safe to call every frame.
    void poll(double nowSec);

    // Send the VJ's performance controls. Rate-limited internally to ~10 Hz,
    // so calling it every frame is fine.
    void sendControl(bool holdArmed, bool windowOpen, double nowSec);

    const CrowdState& state() const { return m_state; }

private:
    CrowdState m_state;
    // Raw socket handles, kept as intptr_t so this header does not drag
    // <winsock2.h> into main.cpp.
    intptr_t m_recvSock = -1;
    intptr_t m_sendSock = -1;
    uint16_t m_controlPort = kCrowdControlPort;
    int      m_lastSeq = -1;
    uint16_t m_controlSeq = 0;
    double   m_lastControlSentAt = -1.0;
};

// How much of each axis the crowd is allowed to move. Two separate weights:
// `level` rides the gauge continuously, `hit` only lands on the burst.
struct CrowdWeights {
    float levelGeometry = 0.6f, hitGeometry = 1.0f;
    float levelColor    = 0.5f, hitColor    = 0.8f;
    // Stage 2 — off until the first two have been judged on a real screen.
    float levelChance   = 0.4f, hitChance   = 1.0f;
    float levelTexture  = 0.3f, hitTexture  = 0.7f;
    float levelChaos    = 0.2f, hitChaos    = 0.8f;
    // These two empty the screen out. Never on the continuous level, and
    // only on the burst once someone has decided it looks good.
    float hitMissing    = 0.5f;
    float hitDepth      = 0.5f;
};

// Blend the crowd's contribution into params the VJ (and AutoMode) already
// set. Deliberately not `x += amount`: adding saturates against the 0..1
// clamp, so whenever the VJ sits high the audience would appear to do
// nothing. Moving a fraction of the *remaining headroom* always shows.
// MASTER is never touched — that stays the VJ's.
vj::Params applyCrowd(const vj::Params& in,
                      float level, float hit, float cap,
                      bool stage2, bool allowMissingDepth,
                      const CrowdWeights& w = CrowdWeights{});

}  // namespace vjmix
