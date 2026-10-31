#include "SpatialIndex.h"
#include "MortonCode.h"
#include <cmath>
#include <algorithm>

void SpatialIndex::insert(const SpatialRecord& record) {
    uint32_t x_int = quantize_coord(record.x);
    uint32_t y_int = quantize_coord(record.y);
    uint32_t z_int = quantize_coord(record.z);

    uint64_t z_key = calculate_z_order(x_int, y_int, z_int);
    btree_index[z_key] = record;
}

std::vector<SpatialRecord> SpatialIndex::range_query(float x_min, float x_max, float y_min, float y_max, float z_min, float z_max) {
    std::vector<SpatialRecord> matches;

    uint64_t min_key = calculate_z_order(quantize_coord(x_min), quantize_coord(y_min), quantize_coord(z_min));
    uint64_t max_key = calculate_z_order(quantize_coord(x_max), quantize_coord(y_max), quantize_coord(z_max));

    // quantize_coord is monotone, so min corner key <= max corner key whenever min <= max per axis
    if (min_key > max_key) return matches;

    auto it_low = btree_index.lower_bound(min_key);
    auto it_high = btree_index.upper_bound(max_key);

    for (auto it = it_low; it != it_high; ++it) {
        const SpatialRecord& rec = it->second;
        if (rec.x >= x_min && rec.x <= x_max &&
            rec.y >= y_min && rec.y <= y_max &&
            rec.z >= z_min && rec.z <= z_max) {
            matches.push_back(rec);
        }
    }
    return matches;
}