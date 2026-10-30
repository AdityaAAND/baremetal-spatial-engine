#include "WriteAheadLog.h"
#include "SpatialRecord.h"
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdexcept>

WriteAheadLog::WriteAheadLog(const char* filepath, size_t size_in_bytes) 
    : file_size(size_in_bytes), current_offset(0) {
    
    fd = open(filepath, O_RDWR | O_CREAT, 0666);
    if (fd == -1) throw std::runtime_error("Failed to open WAL file");

    ftruncate(fd, file_size);

    mapped_memory = (SpatialRecord*)mmap(nullptr, file_size, 
                                         PROT_READ | PROT_WRITE, 
                                         MAP_SHARED, fd, 0);
    
    if (mapped_memory == MAP_FAILED) {
        close(fd);
        throw std::runtime_error("mmap failed");
    }
}

void WriteAheadLog::append(const SpatialRecord& record) {
    size_t index = current_offset.fetch_add(1, std::memory_order_relaxed);
    mapped_memory[index] = record;
}

WriteAheadLog::~WriteAheadLog() {
    msync(mapped_memory, file_size, MS_SYNC);
    munmap(mapped_memory, file_size);
    close(fd);
}