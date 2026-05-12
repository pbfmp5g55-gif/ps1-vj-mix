// Shared-memory SPSC ring for live primitive streaming between a pcsx-redux
// fork and the mixer. Windows-first implementation; POSIX behind the same
// interface comes later.
//
// Wire format inside the ring:
//   record: uint32_t length          (incl. this header)
//           uint8_t  type            (0=Primitive, 1=VRAMUpload, 2=FrameEnd)
//           payload[length - 5]
//
// SPSC discipline: writer reads its own writeOffset from local state,
// reader reads it from the segment. Both wrap at dataSize. Writer never
// overwrites unread bytes: if a record won't fit, the writer drops it (or
// the whole frame, per the higher-level convention) and bumps a drop
// counter. Frame integrity is the caller's responsibility.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace vjmix {

constexpr uint32_t kRingMagic    = 0x47524A56u;  // 'VJRG' little-endian
constexpr uint32_t kRingVersion  = 1u;
constexpr size_t   kDefaultRingDataSize = 2u * 1024u * 1024u;  // 2 MB payload

enum class IpcRecordType : uint8_t {
    Primitive  = 0,
    VRAMUpload = 1,
    FrameEnd   = 2,
};

// Header sits at the start of the shared-memory mapping. data[] follows it.
struct RingHeader {
    uint32_t magic;        // kRingMagic when the segment is alive
    uint32_t version;      // kRingVersion
    uint64_t writeOffset;  // bytes from data[0], advances modulo dataSize
    uint64_t readOffset;   // bytes from data[0]
    uint32_t dataSize;     // total payload bytes available
    uint32_t writerAlive;  // bumped by the writer each frame; reader detects
                           // hangs by watching this stagnate
    uint32_t dropped;      // records dropped due to backpressure
    uint32_t _reserved;
};
static_assert(sizeof(RingHeader) == 40, "RingHeader unexpected size");

class IpcRingWriter {
   public:
    IpcRingWriter() = default;
    ~IpcRingWriter();

    IpcRingWriter(const IpcRingWriter&)            = delete;
    IpcRingWriter& operator=(const IpcRingWriter&) = delete;

    // Create (or recreate) a named ring with the given payload size.
    // Truncates / reinitialises if a stale segment with that name exists.
    bool create(const std::string& name, size_t dataSize = kDefaultRingDataSize);
    void close();
    bool isOpen() const { return m_header != nullptr; }

    // Append one record. Returns true if accepted. On contention with the
    // reader, returns false and increments dropped counter.
    bool writeRecord(IpcRecordType type, const void* payload, size_t payloadLen);

    // Bump the heartbeat so readers can tell the writer is alive.
    void heartbeat();

   private:
    void* m_mapping = nullptr;        // HANDLE on Windows
    void* m_view    = nullptr;        // mapped base address
    RingHeader*    m_header = nullptr;
    uint8_t*       m_data   = nullptr;
    size_t         m_dataSize = 0;
    std::string    m_name;
};

class IpcRingReader {
   public:
    IpcRingReader() = default;
    ~IpcRingReader();

    IpcRingReader(const IpcRingReader&)            = delete;
    IpcRingReader& operator=(const IpcRingReader&) = delete;

    // Attach to an existing ring by name. Fails if the segment doesn't
    // exist or has the wrong magic.
    bool open(const std::string& name);
    void close();
    bool isOpen() const { return m_header != nullptr; }

    // Try to read one record. If a full record is available, fills the
    // out fields and advances readOffset; otherwise returns false without
    // mutating state. payloadBuf is filled with up to maxLen bytes; the
    // record's true length is returned via outLen.
    bool readRecord(IpcRecordType& outType,
                    void* payloadBuf, size_t maxLen, size_t& outLen);

    uint32_t droppedCount() const { return m_header ? m_header->dropped : 0; }
    uint32_t writerHeartbeat() const { return m_header ? m_header->writerAlive : 0; }
    size_t   dataSize() const { return m_dataSize; }

   private:
    void* m_mapping = nullptr;
    void* m_view    = nullptr;
    RingHeader* m_header = nullptr;
    uint8_t*    m_data   = nullptr;
    size_t      m_dataSize = 0;
    std::string m_name;
};

}  // namespace vjmix
