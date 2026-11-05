#include "WriteAheadLog.h"
#include "SpatialRecord.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <stdexcept>

static constexpr uint64_t WAL_MAGIC = 0x4D4F52544F4E5741ULL; // "MORTONWA"
static constexpr uint32_t WAL_VERSION = 1;

WriteAheadLog::WriteAheadLog(const char* filepath, size_t size_in_bytes)
    : fd(-1), mapped_memory(nullptr), file_size(size_in_bytes), capacity(0), header(nullptr) {

    fd = open(filepath, O_RDWR | O_CREAT, 0666);
    if (fd == -1) throw std::runtime_error("Failed to open WAL file");

    struct stat st{};
    fstat(fd, &st);
    // Never shrink an existing log: truncating would cut off entries
    if (static_cast<size_t>(st.st_size) > file_size) file_size = static_cast<size_t>(st.st_size);
    if (file_size < sizeof(WalHeader) + sizeof(LogEntry)) {
        close(fd);
        throw std::runtime_error("WAL size too small");
    }
    if (ftruncate(fd, file_size) != 0) {
        close(fd);
        throw std::runtime_error("Failed to size WAL file");
    }

    mapped_memory = (char*)mmap(nullptr, file_size,
                                PROT_READ | PROT_WRITE,
                                MAP_SHARED, fd, 0);

    if (mapped_memory == MAP_FAILED) {
        close(fd);
        throw std::runtime_error("mmap failed");
    }

    header = reinterpret_cast<WalHeader*>(mapped_memory);
    capacity = (file_size - sizeof(WalHeader)) / sizeof(LogEntry);

    if (header->magic == WAL_MAGIC && header->version == WAL_VERSION) {
        if (header->entry_count > capacity) header->entry_count = capacity; // guard against a corrupt count
    } else {
        if (st.st_size > 0) {
            std::cerr << "[WAL] " << filepath << ": unrecognised format, reinitialising (old contents discarded)\n";
        }
        header->magic = WAL_MAGIC;
        header->version = WAL_VERSION;
        header->entry_count = 0;
    }
}

bool WriteAheadLog::append_entry(const LogEntry& entry) {
    std::lock_guard<std::mutex> lock(append_mutex);
    uint64_t n = header->entry_count;
    if (n >= capacity) return false;

    std::memcpy(mapped_memory + sizeof(WalHeader) + n * sizeof(LogEntry), &entry, sizeof(LogEntry));
    header->entry_count = n + 1; // publish only after the entry is fully written
    return true;
}

bool WriteAheadLog::append(const SpatialRecord& record) {
    LogEntry entry{};
    entry.type = LOG_DATA;
    entry.rec = record;
    return append_entry(entry);
}

bool WriteAheadLog::append_remove_range(uint64_t min_key, uint64_t max_key) {
    LogEntry entry{};
    entry.type = LOG_REMOVE_RANGE;
    entry.tx_id = min_key;
    entry.rec.timestamp = max_key;
    return append_entry(entry);
}

size_t WriteAheadLog::size() const {
    std::lock_guard<std::mutex> lock(append_mutex);
    return header->entry_count;
}

void WriteAheadLog::for_each(const std::function<void(const LogEntry&)>& fn) const {
    size_t n = size();
    for (size_t i = 0; i < n; ++i) {
        LogEntry entry;
        std::memcpy(&entry, mapped_memory + sizeof(WalHeader) + i * sizeof(LogEntry), sizeof(LogEntry));
        fn(entry);
    }
}

WriteAheadLog::~WriteAheadLog() {
    msync(mapped_memory, file_size, MS_SYNC);
    munmap(mapped_memory, file_size);
    close(fd);
}
