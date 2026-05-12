#include "mixer/ipc/ipc_ring.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX  // keep windows.h from #defining min/max as macros
#endif
#include <windows.h>
#endif

#include <algorithm>  // std::min

namespace vjmix {

namespace {

// Record header on the wire: 4-byte length then 1-byte type, then payload.
constexpr size_t kRecHeaderBytes = 5;

// Compute available bytes between writeOffset and readOffset (writer view).
size_t availableForWriter(uint64_t writeOff, uint64_t readOff, size_t dataSize) {
    // We leave one byte unused so a full ring is distinguishable from empty.
    const uint64_t used = (writeOff - readOff + dataSize) % dataSize;
    return dataSize - 1 - static_cast<size_t>(used);
}

// Bytes available to read (reader view).
size_t availableForReader(uint64_t writeOff, uint64_t readOff, size_t dataSize) {
    return static_cast<size_t>((writeOff - readOff + dataSize) % dataSize);
}

void copyOutWrap(uint8_t* dst, const uint8_t* src, size_t offset,
                 size_t dataSize, size_t len) {
    const size_t firstChunk = std::min(len, dataSize - offset);
    std::memcpy(dst, src + offset, firstChunk);
    if (firstChunk < len) {
        std::memcpy(dst + firstChunk, src, len - firstChunk);
    }
}

void copyInWrap(uint8_t* dst, size_t offset, size_t dataSize,
                const uint8_t* src, size_t len) {
    const size_t firstChunk = std::min(len, dataSize - offset);
    std::memcpy(dst + offset, src, firstChunk);
    if (firstChunk < len) {
        std::memcpy(dst, src + firstChunk, len - firstChunk);
    }
}

#ifdef _WIN32
struct PlatformMap {
    HANDLE mapping = nullptr;
    void*  view    = nullptr;
};

bool platformCreate(const std::string& name, size_t totalBytes, PlatformMap& out) {
    out.mapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, static_cast<DWORD>(totalBytes),
        std::wstring(name.begin(), name.end()).c_str());
    if (!out.mapping) return false;
    out.view = MapViewOfFile(out.mapping, FILE_MAP_ALL_ACCESS, 0, 0, totalBytes);
    if (!out.view) {
        CloseHandle(out.mapping);
        out.mapping = nullptr;
        return false;
    }
    return true;
}

bool platformOpen(const std::string& name, size_t totalBytes, PlatformMap& out) {
    out.mapping = OpenFileMappingW(
        FILE_MAP_ALL_ACCESS, FALSE,
        std::wstring(name.begin(), name.end()).c_str());
    if (!out.mapping) return false;
    out.view = MapViewOfFile(out.mapping, FILE_MAP_ALL_ACCESS, 0, 0, totalBytes);
    if (!out.view) {
        CloseHandle(out.mapping);
        out.mapping = nullptr;
        return false;
    }
    return true;
}

void platformClose(PlatformMap& m) {
    if (m.view)    { UnmapViewOfFile(m.view); m.view = nullptr; }
    if (m.mapping) { CloseHandle(m.mapping);  m.mapping = nullptr; }
}
#endif

}  // namespace

// ============================================================================
// IpcRingWriter
// ============================================================================

IpcRingWriter::~IpcRingWriter() { close(); }

bool IpcRingWriter::create(const std::string& name, size_t dataSize) {
    close();
#ifdef _WIN32
    const size_t totalBytes = sizeof(RingHeader) + dataSize;
    PlatformMap pm;
    if (!platformCreate(name, totalBytes, pm)) {
        std::fprintf(stderr, "[ipc] CreateFileMappingW failed: %lu\n",
                     GetLastError());
        return false;
    }
    m_mapping = pm.mapping;
    m_view    = pm.view;
    auto* hdr = static_cast<RingHeader*>(pm.view);
    hdr->magic       = kRingMagic;
    hdr->version     = kRingVersion;
    hdr->writeOffset = 0;
    hdr->readOffset  = 0;
    hdr->dataSize    = static_cast<uint32_t>(dataSize);
    hdr->writerAlive = 0;
    hdr->dropped     = 0;
    hdr->_reserved   = 0;
    m_header   = hdr;
    m_data     = reinterpret_cast<uint8_t*>(hdr) + sizeof(RingHeader);
    m_dataSize = dataSize;
    m_name     = name;
    return true;
#else
    (void)name;
    (void)dataSize;
    std::fprintf(stderr, "[ipc] POSIX implementation not built yet\n");
    return false;
#endif
}

void IpcRingWriter::close() {
#ifdef _WIN32
    if (m_view || m_mapping) {
        PlatformMap pm{m_mapping, m_view};
        platformClose(pm);
        m_mapping = pm.mapping;
        m_view    = pm.view;
    }
#endif
    m_header   = nullptr;
    m_data     = nullptr;
    m_dataSize = 0;
}

bool IpcRingWriter::writeRecord(IpcRecordType type, const void* payload, size_t payloadLen) {
    if (!m_header) return false;
    const size_t recBytes = kRecHeaderBytes + payloadLen;
    if (recBytes > m_dataSize) {
        // Pathologically large record; bump drop and bail.
        m_header->dropped += 1;
        return false;
    }
    const uint64_t writeOff = m_header->writeOffset;
    const uint64_t readOff  = m_header->readOffset;
    if (availableForWriter(writeOff, readOff, m_dataSize) < recBytes) {
        m_header->dropped += 1;
        return false;
    }
    uint8_t hdr[kRecHeaderBytes];
    const uint32_t lenWire = static_cast<uint32_t>(recBytes);
    std::memcpy(&hdr[0], &lenWire, sizeof(lenWire));
    hdr[4] = static_cast<uint8_t>(type);
    copyInWrap(m_data, static_cast<size_t>(writeOff), m_dataSize, hdr, kRecHeaderBytes);
    if (payloadLen > 0) {
        copyInWrap(m_data,
                   static_cast<size_t>((writeOff + kRecHeaderBytes) % m_dataSize),
                   m_dataSize,
                   static_cast<const uint8_t*>(payload), payloadLen);
    }
    // Publish the new writeOffset last — on x86 with atomic uint64 stores
    // this is enough for an in-process SPSC selftest; cross-process on
    // x86 also works for naturally-aligned stores.
    m_header->writeOffset = (writeOff + recBytes) % m_dataSize;
    return true;
}

void IpcRingWriter::heartbeat() {
    if (m_header) m_header->writerAlive += 1;
}

// ============================================================================
// IpcRingReader
// ============================================================================

IpcRingReader::~IpcRingReader() { close(); }

bool IpcRingReader::open(const std::string& name) {
    close();
#ifdef _WIN32
    // We need to map enough to see the header, then re-map with the real
    // data size.
    PlatformMap probe;
    if (!platformOpen(name, sizeof(RingHeader), probe)) return false;
    auto* hdr = static_cast<RingHeader*>(probe.view);
    if (hdr->magic != kRingMagic || hdr->version != kRingVersion) {
        platformClose(probe);
        return false;
    }
    const size_t fullBytes = sizeof(RingHeader) + hdr->dataSize;
    platformClose(probe);

    PlatformMap full;
    if (!platformOpen(name, fullBytes, full)) return false;
    m_mapping  = full.mapping;
    m_view     = full.view;
    m_header   = static_cast<RingHeader*>(full.view);
    m_data     = reinterpret_cast<uint8_t*>(m_header) + sizeof(RingHeader);
    m_dataSize = m_header->dataSize;
    m_name     = name;
    return true;
#else
    (void)name;
    return false;
#endif
}

void IpcRingReader::close() {
#ifdef _WIN32
    if (m_view || m_mapping) {
        PlatformMap pm{m_mapping, m_view};
        platformClose(pm);
        m_mapping = pm.mapping;
        m_view    = pm.view;
    }
#endif
    m_header   = nullptr;
    m_data     = nullptr;
    m_dataSize = 0;
}

bool IpcRingReader::readRecord(IpcRecordType& outType,
                               void* payloadBuf, size_t maxLen, size_t& outLen) {
    if (!m_header) return false;
    const uint64_t writeOff = m_header->writeOffset;
    const uint64_t readOff  = m_header->readOffset;
    const size_t avail = availableForReader(writeOff, readOff, m_dataSize);
    if (avail < kRecHeaderBytes) return false;

    uint8_t hdr[kRecHeaderBytes];
    copyOutWrap(hdr, m_data, static_cast<size_t>(readOff), m_dataSize, kRecHeaderBytes);
    uint32_t recBytes = 0;
    std::memcpy(&recBytes, &hdr[0], sizeof(recBytes));
    if (recBytes < kRecHeaderBytes || recBytes > m_dataSize) return false;
    if (avail < recBytes) return false;  // record only partially written

    outType = static_cast<IpcRecordType>(hdr[4]);
    const size_t payloadLen = recBytes - kRecHeaderBytes;
    outLen = payloadLen;
    if (payloadLen > 0) {
        const size_t copyLen = std::min(payloadLen, maxLen);
        copyOutWrap(static_cast<uint8_t*>(payloadBuf), m_data,
                    static_cast<size_t>((readOff + kRecHeaderBytes) % m_dataSize),
                    m_dataSize, copyLen);
    }
    m_header->readOffset = (readOff + recBytes) % m_dataSize;
    return true;
}

}  // namespace vjmix
