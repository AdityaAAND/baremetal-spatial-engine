#pragma once
#include <cstdint>

struct ShardRange{
    uint32_t node_id;
    uint64_t start_key;
    uint64_t end_key;

};
