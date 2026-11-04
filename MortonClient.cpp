#include "MortonClient.h"

SpatialQueryBuilder::SpatialQueryBuilder(Coordinator& coordinator_ref) 
    : coordinator(coordinator_ref), req{0, 0, 0, 0, 0, 0}, bbox_set(false) {}

SpatialQueryBuilder& SpatialQueryBuilder::bounding_box(float x_min, float x_max, float y_min, float y_max, float z_min, float z_max) {
    req = {x_min, x_max, y_min, y_max, z_min, z_max};
    bbox_set = true;
    return *this;
}

QueryResult SpatialQueryBuilder::execute() {
    QueryResult result{};
    if (!bbox_set) {
        result.plan.plan_type = ExecutionPlanType::INVALID_PLAN;
        result.error = "bounding box not specified";
        return result;
    }

    result.plan = QueryOptimizer::optimize(req);
    if (result.plan.plan_type == ExecutionPlanType::INVALID_PLAN) {
        result.error = result.plan.description;
        return result;
    }

    // Sub-boxes are disjoint, so concatenating their rows cannot produce duplicates
    for (const auto& sub_query : result.plan.sub_queries) {
        if (!coordinator.execute_query(sub_query, result.records)) {
            // Rows from the shards that did answer would pass for a complete result
            result.records.clear();
            result.error = "a shard failed to answer the query";
            return result;
        }
    }

    result.ok = true;
    return result;
}

MortonClient::MortonClient(Coordinator& coordinator_ref) : coordinator(coordinator_ref) {}

bool MortonClient::insert(const SpatialRecord& record) {
    return coordinator.execute_2pc_transaction(coordinator.next_tx_id(), {record});
}

bool MortonClient::insert(float x, float y, float z, uint64_t timestamp, uint32_t metadata) {
    SpatialRecord rec = {timestamp, x, y, z, metadata};
    return insert(rec);
}

SpatialQueryBuilder MortonClient::query() {
    return SpatialQueryBuilder(coordinator);
}
