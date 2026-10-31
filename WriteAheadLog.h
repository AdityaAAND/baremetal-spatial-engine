#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include "SpatialRecord.h"

// Kinds of log entries. LOG_DATA is a record applied directly; the tx types let
// recovery rebuild two-phase-commit state that was in flight at crash time.
enum LogType : uint8_t {
    LOG_DATA    = 0x01,
    LOG_PREPARE = 0x02,
    LOG_COMMIT  = 0x03,
    LOG_ABORT   = 0x04
};

#pragma pack(push, 1)
struct LogEntry {
    uint8_t type;      // LogType
    uint64_t tx_id;    // 0 for LOG_DATA
    SpatialRecord rec; // unused for LOG_COMMIT / LOG_ABORT
};

// Stored at offset 0 so a reopened log resumes after its last entry instead of overwriting it
struct WalHeader {
    uint64_t magic;
    uint32_t version;
    uint64_t entry_count;
};
#pragma pack(pop)

class WriteAheadLog{
private:
    int fd;
    char* mapped_memory;
    size_t file_size;
    size_t capacity; // max entries that fit after the header
    WalHeader* header;
    mutable std::mutex append_mutex;
public:
    WriteAheadLog(const char* filepath, size_t size_in_bytes);
    bool append(const SpatialRecord& record); // appends a LOG_DATA entry; false if the log is full
    bool append_entry(const LogEntry& entry);
    size_t size() const;
    void for_each(const std::function<void(const LogEntry&)>& fn) const;
    ~WriteAheadLog();
};
