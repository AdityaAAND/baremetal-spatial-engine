#include "SpatialIndex.h"
#include "MortonCode.h"

void SpatialIndex::insert(const SpatialRecord& record) {
    uint32_t x_int = quantize_coord(record.x);
    uint32_t y_int = quantize_coord(record.y);
    uint32_t z_int = quantize_coord(record.z);

    uint64_t z_key = calculate_z_order(x_int, y_int, z_int);
    btree_index[z_key] = record;
}

std::vector<SpatialRecord> SpatialIndex::range_query(float x_min, float x_max, float y_min, float y_max, float z_min, float z_max) {
    std::vector<SpatialRecord> matches;
    scan_box(x_min, x_max, y_min, y_max, z_min, z_max, [&](const SpatialRecord& rec) {
        matches.push_back(rec);
        return true;
    });
    return matches;
}

void SpatialIndex::scan_box(float x_min, float x_max, float y_min, float y_max, float z_min, float z_max,
                            const std::function<bool(const SpatialRecord&)>& visit) const {
    uint32_t x1 = quantize_coord(x_min);
    uint32_t y1 = quantize_coord(y_min);
    uint32_t z1 = quantize_coord(z_min);

    uint32_t x2 = quantize_coord(x_max);
    uint32_t y2 = quantize_coord(y_max);
    uint32_t z2 = quantize_coord(z_max);

    uint64_t min_key = calculate_z_order(x1, y1, z1);
    uint64_t max_key = calculate_z_order(x2, y2, z2);

    if (min_key > max_key) return;

    auto it_low = btree_index.lower_bound(min_key);
    auto it_high = btree_index.upper_bound(max_key);

    for (auto it = it_low; it != it_high; ++it) {
        const SpatialRecord& rec = it->second;
        if (rec.x >= x_min && rec.x <= x_max &&
            rec.y >= y_min && rec.y <= y_max &&
            rec.z >= z_min && rec.z <= z_max) {
            if (!visit(rec)) return;
        }
    }
}

std::vector<uint64_t> SpatialIndex::keys() const {
    std::vector<uint64_t> result;
    result.reserve(btree_index.size());
    for (const auto& entry : btree_index) result.push_back(entry.first);
    return result;
}

std::vector<SpatialRecord> SpatialIndex::extract_and_remove_range(uint64_t min_key, uint64_t max_key) {
    std::vector<SpatialRecord> extracted;
    auto it_low = btree_index.lower_bound(min_key);
    auto it_high = btree_index.upper_bound(max_key);

    for (auto it = it_low; it != it_high; ) {
        extracted.push_back(it->second);
        it = btree_index.erase(it);
    }
    return extracted;
}