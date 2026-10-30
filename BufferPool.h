#pragma once
#include <vector>
#include <mutex>
#include <cstddef>
#include "SpatialRecord.h"
#include "SpatialIndex.h"//Bring in layer 2 

class BufferPool{
private:
    std::vector<SpatialRecord> cache;
    std::mutex pool_mutex;
    size_t max_capacity;

    SpatialIndex& spatial_index;


    void flush_to_index();
public:
    BufferPool(size_t capacity, SpatialIndex& index);
    void insert(const SpatialRecord& record);
};
