#pragma once 
#include<map>
#include<cstdint>
#include "SpatialRecord.h"

class SpatialIndex{
private:
    std::map<uint64_t, SpatialRecord> btree_index;
public:
    void insert(const SpatialRecord& record);
};