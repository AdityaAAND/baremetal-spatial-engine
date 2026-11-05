#pragma once 
#include<map>
#include <functional>
#include <vector>
#include<cstdint>
#include "SpatialRecord.h"

class SpatialIndex{
private:
    std::map<uint64_t, SpatialRecord> btree_index;
public:
    void insert(const SpatialRecord& record);
    std::vector<SpatialRecord> range_query(float x_min, float x_max, float y_min, float y_max, float z_min, float z_max);
    // Calls visit for each record in the box, in key order, until it returns false.
    // Letting the caller stop early is what makes a query LIMIT end the scan.
    void scan_box(float x_min, float x_max, float y_min, float y_max, float z_min, float z_max,
                  const std::function<bool(const SpatialRecord&)>& visit) const;
    std::vector<SpatialRecord> extract_and_remove_range(uint64_t min_key, uint64_t max_key);
    std::vector<uint64_t> keys() const; // in ascending order
    size_t size() const {return btree_index.size();}
};
