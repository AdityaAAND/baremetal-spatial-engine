#pragma once
#include "NetworkProtocol.h"
#include <cstdint>
#include <string>
#include <vector>

enum class ExecutionPlanType {
    MORTON_INDEX_RANGE_SCAN, // one key range covering the whole box
    MORTON_SPLIT_RANGE_SCAN, // several narrower key ranges, one sub-box each
    INVALID_PLAN
};

struct ExecutionPlan {
    ExecutionPlanType plan_type;
    QueryRequest bounding_box;
    uint64_t key_span;                     // key range one scan of the whole box would cover
    uint64_t scan_cost;                    // total key range this plan covers
    std::vector<QueryRequest> sub_queries; // disjoint sub-boxes to execute; empty for INVALID_PLAN
    std::string description;
};

class QueryOptimizer {
public:
    static ExecutionPlan optimize(const QueryRequest& req);
};
