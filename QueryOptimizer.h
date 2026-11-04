#pragma once
#include "SpatialRecord.h"
#include "NetworkProtocol.h"
#include "MortonCode.h"
#include <string>

enum class ExecutionPlanType {
    MORTON_INDEX_RANGE_SCAN,
    FULL_SHARD_SWEEP,
    INVALID_PLAN
};

struct ExecutionPlan {
    ExecutionPlanType plan_type;
    QueryRequest bounding_box;
    float calculated_volume;
    uint64_t key_span;
    std::string description;
};

class QueryOptimizer {
public:
    static ExecutionPlan optimize(const QueryRequest& req);
};