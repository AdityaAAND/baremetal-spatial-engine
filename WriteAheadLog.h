#pragma once
#include <atomic>
#include<cstddef>
#include "SpatialRecord.h"

class WriteAheadLog{
private:
    int fd;
    SpatialRecord* mapped_memory;
    size_t file_size;
    std::atomic<size_t> current_offset;
public:
    WriteAheadLog(const char* filepath, size_t size_in_bytes);
    void append(const SpatialRecord& record);
    ~WriteAheadLog();
};
