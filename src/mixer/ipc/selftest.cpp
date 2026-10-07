// In-process selftest for vjmix::IpcRingWriter / IpcRingReader.
// Writes a few records, reads them back, checks round-trip equality,
// also exercises wrap-around by pushing more bytes than dataSize across
// multiple write/read cycles.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "mixer/ipc/ipc_ring.h"

#ifdef _WIN32
#include <windows.h>
#endif

// Always-evaluating check (assert() is stripped in NDEBUG / Release builds,
// which would silently elide the writeRecord/readRecord calls).
#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::fprintf(stderr, "[selftest] CHECK failed at %s:%d: %s\n",     \
                         __FILE__, __LINE__, #expr);                           \
            std::abort();                                                      \
        }                                                                     \
    } while (0)

namespace {

void verifyRoundTripBasic() {
    vjmix::IpcRingWriter w;
    CHECK(w.create("Local\\vj-mix-selftest-basic", 4096));
    vjmix::IpcRingReader r;
    CHECK(r.open("Local\\vj-mix-selftest-basic"));

    const char* msg1 = "hello primitive";
    const char* msg2 = "and another";
    CHECK(w.writeRecord(vjmix::IpcRecordType::Primitive, msg1, std::strlen(msg1)));
    CHECK(w.writeRecord(vjmix::IpcRecordType::FrameEnd,  msg2, std::strlen(msg2)));

    vjmix::IpcRecordType t;
    char buf[256];
    size_t len = 0;

    CHECK(r.readRecord(t, buf, sizeof(buf), len));
    CHECK(t == vjmix::IpcRecordType::Primitive);
    CHECK(len == std::strlen(msg1));
    CHECK(std::memcmp(buf, msg1, len) == 0);

    CHECK(r.readRecord(t, buf, sizeof(buf), len));
    CHECK(t == vjmix::IpcRecordType::FrameEnd);
    CHECK(len == std::strlen(msg2));
    CHECK(std::memcmp(buf, msg2, len) == 0);

    // No more records.
    CHECK(!r.readRecord(t, buf, sizeof(buf), len));

    std::puts("[selftest] basic round-trip OK");
}

void verifyWrapAround() {
    // Force several wraps. 1 KB ring with ~120-byte records.
    vjmix::IpcRingWriter w;
    CHECK(w.create("Local\\vj-mix-selftest-wrap", 1024));
    vjmix::IpcRingReader r;
    CHECK(r.open("Local\\vj-mix-selftest-wrap"));

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
        CHECK(w.writeRecord(vjmix::IpcRecordType::Primitive,
                             payload.data(), payload.size()));

        vjmix::IpcRecordType t;
        size_t len = 0;
        CHECK(r.readRecord(t, readBuf.data(), readBuf.size(), len));
        CHECK(t == vjmix::IpcRecordType::Primitive);
        CHECK(len == kPayloadLen);
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
    CHECK(w.create("Local\\vj-mix-selftest-drop", 512));
    vjmix::IpcRingReader r;
    CHECK(r.open("Local\\vj-mix-selftest-drop"));

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
    CHECK(writesAccepted > 0);
    CHECK(writesAccepted < 100);
    CHECK(r.droppedCount() >= static_cast<uint32_t>(100 - writesAccepted));

    std::puts("[selftest] backpressure drop OK (accepted %d / 100)");
    std::printf("            -> writesAccepted=%d droppedCount=%u\n",
                writesAccepted, r.droppedCount());
}

#ifdef _WIN32
void verifyResyncAfterMisalignedRead() {
    // A readOffset that is off a record boundary used to wedge the ring for
    // good: garbage length -> readRecord returns false forever -> ring fills
    // -> every frame dropped -> frozen output. The reader must skip to the
    // writer's position and carry on.
    const char* name = "Local\\vj-mix-selftest-resync";
    vjmix::IpcRingWriter w;
    CHECK(w.create(name, 4096));
    vjmix::IpcRingReader r;
    CHECK(r.open(name));

    std::vector<uint8_t> payload(64, 0x7F);  // 0x7F7F7F7F as a length = garbage
    CHECK(w.writeRecord(vjmix::IpcRecordType::Primitive, payload.data(), payload.size()));
    CHECK(w.writeRecord(vjmix::IpcRecordType::Primitive, payload.data(), payload.size()));

    // Knock readOffset into the middle of the first record's payload.
    HANDLE h = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
    CHECK(h != nullptr);
    auto* hdr = static_cast<vjmix::RingHeader*>(
        MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(vjmix::RingHeader)));
    CHECK(hdr != nullptr);
    hdr->readOffset += 9;

    vjmix::IpcRecordType t;
    std::vector<uint8_t> buf(256);
    size_t len = 0;
    CHECK(!r.readRecord(t, buf.data(), buf.size(), len));
    CHECK(r.resyncCount() == 1);
    CHECK(hdr->readOffset == hdr->writeOffset);

    // Back on a boundary: the next record round-trips.
    const char msg[] = "after";
    CHECK(w.writeRecord(vjmix::IpcRecordType::FrameEnd, msg, sizeof(msg)));
    CHECK(r.readRecord(t, buf.data(), buf.size(), len));
    CHECK(t == vjmix::IpcRecordType::FrameEnd);
    CHECK(len == sizeof(msg) && std::memcmp(buf.data(), msg, len) == 0);
    CHECK(r.resyncCount() == 1);

    UnmapViewOfFile(hdr);
    CloseHandle(h);
    std::puts("[selftest] resync after misaligned read OK");
}
#endif

}  // namespace

int main() {
#ifdef _WIN32
    verifyRoundTripBasic();
    verifyWrapAround();
    verifyBackpressureDrop();
    verifyResyncAfterMisalignedRead();
    std::puts("[selftest] ALL OK");
    return 0;
#else
    std::puts("[selftest] skipped (POSIX implementation pending)");
    return 0;
#endif
}
