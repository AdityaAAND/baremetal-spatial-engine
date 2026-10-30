#include "Coordinator.h"
#include "MortonCode.h"
#include <cmath>

Coordinator::Coordinator() {}

void Coordinator::register_shard(uint32_t node_id, uint64_t start_key, uint64_t end_key) {
    cluster_shards.push_back({node_id, start_key, end_key});
}

uint32_t Coordinator::route_query(float x, float y, float z) {
    uint32_t x_int = static_cast<uint32_t>(std::abs(x * 1000.0f));
    uint32_t y_int = static_cast<uint32_t>(std::abs(y * 1000.0f));
    uint32_t z_int = static_cast<uint32_t>(std::abs(z * 1000.0f));

    uint64_t target_key = calculate_z_order(x_int, y_int, z_int);

    for (const auto& shard : cluster_shards) {
        if (target_key >= shard.start_key && target_key <= shard.end_key) {
            return shard.node_id;
        }
    }

    return 0; // Default fallback shard
}