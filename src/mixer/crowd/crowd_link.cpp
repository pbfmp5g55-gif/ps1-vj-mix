#include "mixer/crowd/crowd_link.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <mswsock.h>
#include <ws2tcpip.h>
#endif

namespace vjmix {
namespace {

// Wire format, mirrored in crowd-server/packet.js. Fields are read out one
// at a time rather than memcpy'd over a struct, so packing/padding on either
// side can never silently shift them.
constexpr uint32_t kStateMagic   = 0x52434a56u;  // 'V','J','C','R'
constexpr uint32_t kControlMagic = 0x43434a56u;  // 'V','J','C','C'
constexpr uint8_t  kVersion      = 1;
constexpr size_t   kStateBytes   = 20;
constexpr size_t   kControlBytes = 8;

constexpr uint8_t kFlagHeld       = 1 << 0;
constexpr uint8_t kFlagHoldArmed  = 1 << 1;
constexpr uint8_t kFlagWindowOpen = 1 << 2;
constexpr uint8_t kFlagCooldown   = 1 << 3;

// Packets stop when the server dies. Hold the last value briefly (a dropped
// datagram must not flicker the screen), then ride down to zero rather than
// cutting, which would read as a glitch of its own.
constexpr double kHoldSec  = 0.4;
constexpr double kFadeSec  = 0.6;
constexpr double kControlIntervalSec = 0.1;
// A restarted server begins its sequence at 0 again. Rejecting anything not
// "newer" would then ignore it until the counter climbed past whatever we had
// already seen — about 25 minutes at 20 Hz. Any real gap in the stream means
// the next valid packet is the current truth, whatever its number says.
constexpr double kSeqResyncSec = 1.0;

uint32_t rdU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t rdU16(const uint8_t* p) {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) |
                                 (static_cast<uint16_t>(p[1]) << 8));
}
float rdF32(const uint8_t* p) {
    uint32_t bits = rdU32(p);
    float f = 0.0f;
    std::memcpy(&f, &bits, 4);
    return f;
}
void wrU32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}
void wrU16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

float clamp01(float x) {
    if (!(x == x)) return 0.0f;   // NaN
    return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

// True when `a` is newer than `b` across a 16-bit wrap.
bool seqNewer(uint16_t a, uint16_t b) {
    return static_cast<int16_t>(static_cast<uint16_t>(a - b)) > 0;
}

#ifdef _WIN32
bool ensureWinsock(char* err, size_t errLen) {
    static bool started = false;
    static bool ok = false;
    if (started) return ok;
    started = true;
    WSADATA wsa;
    const int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (rc != 0) {
        std::snprintf(err, errLen, "WSAStartup failed (%d)", rc);
        return false;
    }
    ok = true;
    return true;
}

// Without this, a UDP socket that has ever sent to a closed port starts
// failing its *receives* with WSAECONNRESET once the ICMP unreachable comes
// back — a Windows-only trap that would break the link the moment the crowd
// server is restarted.
void disableConnReset(SOCKET s) {
    BOOL behaviour = FALSE;
    DWORD returned = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &behaviour, sizeof(behaviour),
             nullptr, 0, &returned, nullptr, nullptr);
}
#endif

}  // namespace

CrowdLink::~CrowdLink() { close(); }

bool CrowdLink::open(uint16_t listenPort, uint16_t controlPort) {
    close();
    m_controlPort = controlPort;
    m_state = CrowdState{};

#ifdef _WIN32
    if (!ensureWinsock(m_state.error, sizeof(m_state.error))) return false;

    SOCKET rx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (rx == INVALID_SOCKET) {
        std::snprintf(m_state.error, sizeof(m_state.error),
                      "socket() failed (%d)", WSAGetLastError());
        return false;
    }
    disableConnReset(rx);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(listenPort);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // loopback only, never the LAN
    if (bind(rx, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        std::snprintf(m_state.error, sizeof(m_state.error),
                      "port %u is already in use (%d)", listenPort, WSAGetLastError());
        closesocket(rx);
        return false;
    }
    // If this fails the socket stays blocking, and the second recv() in
    // poll() would park the render thread for good. Refuse to open instead.
    u_long nonblocking = 1;
    if (ioctlsocket(rx, FIONBIO, &nonblocking) != 0) {
        std::snprintf(m_state.error, sizeof(m_state.error),
                      "could not set non-blocking mode (%d)", WSAGetLastError());
        closesocket(rx);
        return false;
    }

    // A separate socket for sending, so the receive socket never triggers an
    // ICMP unreachable against itself.
    SOCKET tx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (tx == INVALID_SOCKET) {
        std::snprintf(m_state.error, sizeof(m_state.error),
                      "control socket() failed (%d)", WSAGetLastError());
        closesocket(rx);
        return false;
    }
    disableConnReset(tx);
    if (ioctlsocket(tx, FIONBIO, &nonblocking) != 0) {
        std::snprintf(m_state.error, sizeof(m_state.error),
                      "could not set control socket non-blocking (%d)",
                      WSAGetLastError());
        closesocket(rx);
        closesocket(tx);
        return false;
    }

    m_recvSock = static_cast<intptr_t>(rx);
    m_sendSock = static_cast<intptr_t>(tx);
    m_state.socketOpen = true;
    return true;
#else
    (void)listenPort;
    std::snprintf(m_state.error, sizeof(m_state.error),
                  "CROWD link is Windows-only for now");
    return false;
#endif
}

void CrowdLink::close() {
#ifdef _WIN32
    if (m_recvSock != -1) closesocket(static_cast<SOCKET>(m_recvSock));
    if (m_sendSock != -1) closesocket(static_cast<SOCKET>(m_sendSock));
#endif
    m_recvSock = -1;
    m_sendSock = -1;
    m_state.socketOpen = false;
    m_state.freshness = 0.0f;
    m_lastSeq = -1;
}

void CrowdLink::poll(double nowSec) {
#ifdef _WIN32
    if (m_recvSock != -1) {
        uint8_t buf[64];
        // Drain the queue and keep only the newest packet: a backlog must not
        // walk the gauge backwards through stale values.
        for (int guard = 0; guard < 256; ++guard) {
            const int n = recv(static_cast<SOCKET>(m_recvSock),
                               reinterpret_cast<char*>(buf), sizeof(buf), 0);
            if (n <= 0) break;
            if (static_cast<size_t>(n) != kStateBytes) { ++m_state.dropped; continue; }
            if (rdU32(buf) != kStateMagic)             { ++m_state.dropped; continue; }
            if (buf[4] != kVersion)                    { ++m_state.dropped; continue; }
            const uint16_t seq = rdU16(buf + 6);
            const bool resync =
                m_state.lastPacketAt < 0.0 ||
                (nowSec - m_state.lastPacketAt) > kSeqResyncSec;
            if (!resync && m_lastSeq >= 0 &&
                !seqNewer(seq, static_cast<uint16_t>(m_lastSeq))) {
                ++m_state.dropped;
                continue;
            }
            if (resync) ++m_state.resyncs;
            m_lastSeq = static_cast<int>(seq);
            const uint8_t flags = buf[5];
            m_state.charge     = clamp01(rdF32(buf + 8));
            m_state.burst      = clamp01(rdF32(buf + 12));
            m_state.active     = static_cast<int>(rdU16(buf + 16));
            m_state.cooldown   = static_cast<float>(rdU16(buf + 18)) / 100.0f;
            m_state.held       = (flags & kFlagHeld) != 0;
            m_state.inCooldown = (flags & kFlagCooldown) != 0;
            m_state.lastPacketAt = nowSec;
            ++m_state.accepted;
        }
    }
#endif

    // Freshness ramp — the only thing that runs when the link is dead.
    if (m_state.lastPacketAt < 0.0) {
        m_state.freshness = 0.0f;
    } else {
        const double age = nowSec - m_state.lastPacketAt;
        if (age <= kHoldSec) {
            m_state.freshness = 1.0f;
        } else if (age >= kHoldSec + kFadeSec) {
            m_state.freshness = 0.0f;
        } else {
            m_state.freshness =
                1.0f - static_cast<float>((age - kHoldSec) / kFadeSec);
        }
    }
}

void CrowdLink::sendControl(bool holdArmed, bool windowOpen, double nowSec) {
#ifdef _WIN32
    if (m_sendSock == -1) return;
    if (m_lastControlSentAt >= 0.0 &&
        nowSec - m_lastControlSentAt < kControlIntervalSec) {
        return;
    }
    m_lastControlSentAt = nowSec;

    uint8_t buf[kControlBytes];
    wrU32(buf, kControlMagic);
    buf[4] = kVersion;
    uint8_t flags = 0;
    if (holdArmed)  flags |= kFlagHoldArmed;
    if (windowOpen) flags |= kFlagWindowOpen;
    buf[5] = flags;
    wrU16(buf + 6, ++m_controlSeq);

    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(m_controlPort);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    // Failure here is expected and uninteresting while the server is down.
    sendto(static_cast<SOCKET>(m_sendSock), reinterpret_cast<const char*>(buf),
           static_cast<int>(sizeof(buf)), 0,
           reinterpret_cast<sockaddr*>(&to), sizeof(to));
#else
    (void)holdArmed; (void)windowOpen; (void)nowSec;
#endif
}

vj::Params applyCrowd(const vj::Params& in,
                      float level, float hit, float cap,
                      bool stage2, bool allowMissingDepth,
                      const CrowdWeights& w) {
    vj::Params out = in;
    level = clamp01(level);
    hit   = clamp01(hit);
    cap   = clamp01(cap);
    if (cap <= 0.0f) return out;

    // Move a share of what is left rather than adding: at in=0.8 an added
    // 0.4 would clamp to 1.0 and every value above 0.6 would look identical.
    const auto blend = [&](float& x, float levelW, float hitW) {
        const float amount = clamp01(cap * (levelW * level + hitW * hit));
        x = x + (1.0f - x) * amount;
    };

    blend(out.geometry, w.levelGeometry, w.hitGeometry);
    blend(out.color,    w.levelColor,    w.hitColor);
    if (stage2) {
        blend(out.chance,  w.levelChance,  w.hitChance);
        blend(out.texture, w.levelTexture, w.hitTexture);
        blend(out.chaos,   w.levelChaos,   w.hitChaos);
    }
    if (allowMissingDepth) {
        // Level weight is deliberately zero: dropping primitives continuously
        // empties the screen, and an audience that cannot see anything stops
        // playing. Only the burst is allowed to do it, briefly.
        blend(out.missing, 0.0f, w.hitMissing);
        blend(out.depth,   0.0f, w.hitDepth);
    }
    // MASTER is untouched on purpose.
    return out;
}

}  // namespace vjmix
