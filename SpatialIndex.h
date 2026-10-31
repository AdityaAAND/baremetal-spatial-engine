#pragma once 
#include<map>
#include <vector>
#include<cstdint>
#include "SpatialRecord.h"

class SpatialIndex{
private:
    std::map<uint64_t, SpatialRecord> btree_index;
public:
    void insert(const SpatialRecord& record);
    std::vector<SpatialRecord> range_query(float x_min, float x_max, float y_min, float y_max, float z_min, float z_max);
    std::vector<SpatialRecord> extract_and_remove_range(uint64_t min_key, uint64_t max_key);
    size_t size() const {return btree_index.size();}
};
