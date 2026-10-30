#include "SpatialIndex.h"
#include "MortonCode.h"
#include <cmath>

void SpatialIndex::insert(const SpatialRecord& record) {
    // Quantize floats into positive integers. 
    // Multiplying by 1000 preserves millimeter-level precision.
    uint32_t x_int = static_cast<uint32_t>(std::abs(record.x * 1000.0f));
    uint32_t y_int = static_cast<uint32_t>(std::abs(record.y * 1000.0f));
    uint32_t z_int = static_cast<uint32_t>(std::abs(record.z * 1000.0f));

    // Generate the 1D spatial key
    uint64_t z_key = calculate_z_order(x_int, y_int, z_int);
    
    // Insert into the tree. 
    btree_index[z_key] = record;
}

