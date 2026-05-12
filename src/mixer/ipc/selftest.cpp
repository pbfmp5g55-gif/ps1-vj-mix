// In-process selftest for vjmix::IpcRingWriter / IpcRingReader.
// Writes a few records, reads them back, checks round-trip equality,
// also exercises wrap-around by pushing more bytes than dataSize across
// multiple write/read cycles.

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "mixer/ipc/ipc_ring.h"

namespace {

void verifyRoundTripBasic() {
    vjmix::IpcRingWriter w;
    assert(w.create("Local\\vj-mix-selftest-basic", 4096));
    vjmix::IpcRingReader r;
    assert(r.open("Local\\vj-mix-selftest-basic"));

    const char* msg1 = "hello primitive";
    const char* msg2 = "and another";
    assert(w.writeRecord(vjmix::IpcRecordType::Primitive, msg1, std::strlen(msg1)));
    assert(w.writeRecord(vjmix::IpcRecordType::FrameEnd,  msg2, std::strlen(msg2)));

    vjmix::IpcRecordType t;
    char buf[256];
    size_t len = 0;

    assert(r.readRecord(t, buf, sizeof(buf), len));
    assert(t == vjmix::IpcRecordType::Primitive);
    assert(len == std::strlen(msg1));
    assert(std::memcmp(buf, msg1, len) == 0);

    assert(r.readRecord(t, buf, sizeof(buf), len));
    assert(t == vjmix::IpcRecordType::FrameEnd);
    assert(len == std::strlen(msg2));
    assert(std::memcmp(buf, msg2, len) == 0);

    // No more records.
    assert(!r.readRecord(t, buf, sizeof(buf), len));

    std::puts("[selftest] basic round-trip OK");
}

void verifyWrapAround() {
    // Force several wraps. 1 KB ring with ~120-byte records.
    vjmix::IpcRingWriter w;
    assert(w.create("Local\\vj-mix-selftest-wrap", 1024));
    vjmix::IpcRingReader r;
    assert(r.open("Local\\vj-mix-selftest-wrap"));

    constexpr size_t kPayloadLen = 119;  // + 5 header = 124 record bytes
    std::vector<uint8_t> payload(kPayloadLen);
    std::vector<uint8_t> readBuf(kPayloadLen + 16);

    // Push and consume one record at a time, 200 times. Each record's
    // payload bytes encode an incrementing counter so we can verify
    // ordering.
    for (int i = 0; i < 200; ++i) {
        for (size_t j = 0; j < kPayloadLen; ++j) {
            payload[j] = static_cast<uint8_t>((i * 7 + j) & 0xff);
        }
        assert(w.writeRecord(vjmix::IpcRecordType::Primitive,
                             payload.data(), payload.size()));

        vjmix::IpcRecordType t;
        size_t len = 0;
        assert(r.readRecord(t, readBuf.data(), readBuf.size(), len));
        assert(t == vjmix::IpcRecordType::Primitive);
        assert(len == kPayloadLen);
        for (size_t j = 0; j < kPayloadLen; ++j) {
            const uint8_t want = static_cast<uint8_t>((i * 7 + j) & 0xff);
            if (readBuf[j] != want) {
                std::fprintf(stderr, "iter %d byte %zu: got %u want %u\n",
                             i, j, readBuf[j], want);
                std::abort();
            }
        }
    }

    std::puts("[selftest] wrap-around (200 records, 1 KB ring) OK");
}

void verifyBackpressureDrop() {
    // Fill the ring without reading; subsequent writes should drop.
    vjmix::IpcRingWriter w;
    assert(w.create("Local\\vj-mix-selftest-drop", 512));
    vjmix::IpcRingReader r;
    assert(r.open("Local\\vj-mix-selftest-drop"));

    constexpr size_t kPayload = 119;
    std::vector<uint8_t> payload(kPayload, 0xCC);
    int writesAccepted = 0;
    for (int i = 0; i < 100; ++i) {
        if (w.writeRecord(vjmix::IpcRecordType::Primitive,
                          payload.data(), payload.size())) {
            ++writesAccepted;
        }
    }
    // 512-byte ring fits roughly 4 records of 124 bytes each (with the
    // one-byte spare). Expect a small number of accepts and the rest
    // dropped.
    assert(writesAccepted > 0);
    assert(writesAccepted < 100);
    assert(r.droppedCount() >= static_cast<uint32_t>(100 - writesAccepted));

    std::puts("[selftest] backpressure drop OK (accepted %d / 100)");
    std::printf("            -> writesAccepted=%d droppedCount=%u\n",
                writesAccepted, r.droppedCount());
}

}  // namespace

int main() {
#ifdef _WIN32
    verifyRoundTripBasic();
    verifyWrapAround();
    verifyBackpressureDrop();
    std::puts("[selftest] ALL OK");
    return 0;
#else
    std::puts("[selftest] skipped (POSIX implementation pending)");
    return 0;
#endif
}
