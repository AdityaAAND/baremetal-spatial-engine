#pragma once
#include <vector>
#include <cstdint>
#include "NodeConfig.h"

class Coordinator {
private:
    std::vector<ShardRange> cluster_shards;

public:
    Coordinator();
    void register_shard(uint32_t node_id, uint64_t start_key, uint64_t end_key);
    uint32_t route_query(float x, float y, float z);
};
