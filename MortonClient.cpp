#include "MortonClient.h"
#include <algorithm>
#include <limits>

SpatialQueryBuilder::SpatialQueryBuilder(Coordinator& coordinator_ref) 
    : coordinator(coordinator_ref), req{0, 0, 0, 0, 0, 0}, bbox_set(false), max_rows(0) {}

SpatialQueryBuilder& SpatialQueryBuilder::bounding_box(float x_min, float x_max, float y_min, float y_max, float z_min, float z_max) {
    req = {x_min, x_max, y_min, y_max, z_min, z_max};
    bbox_set = true;
    return *this;
}

SpatialQueryBuilder& SpatialQueryBuilder::all() {
    constexpr float max = std::numeric_limits<float>::max();
    return bounding_box(-max, max, -max, max, -max, max);
}

SpatialQueryBuilder& SpatialQueryBuilder::where(FilterOp op, uint32_t value, uint32_t mask) {
    filters.push_back({static_cast<uint8_t>(op), value, mask});
    return *this;
}

SpatialQueryBuilder& SpatialQueryBuilder::limit(uint32_t rows) {
    max_rows = rows;
    return *this;
}

QueryResult SpatialQueryBuilder::execute() {
    return run(false);
}

QueryResult SpatialQueryBuilder::execute_count() {
    return run(true);
}

QueryResult SpatialQueryBuilder::run(bool count_only) {
    QueryResult result{};
    if (!bbox_set) {
        result.plan.plan_type = ExecutionPlanType::INVALID_PLAN;
        result.error = "bounding box not specified";
        return result;
    }
    if (filters.size() > MAX_QUERY_FILTERS) {
        result.plan.plan_type = ExecutionPlanType::INVALID_PLAN;
        result.error = "too many filters";
        return result;
    }

    result.plan = QueryOptimizer::optimize(req);
    if (result.plan.plan_type == ExecutionPlanType::INVALID_PLAN) {
        result.error = result.plan.description;
        return result;
    }

    FilteredQueryRequest request{};
    request.count_only = count_only ? 1 : 0;
    request.filter_count = static_cast<uint8_t>(filters.size());
    std::copy(filters.begin(), filters.end(), request.filters);

    // Sub-boxes are disjoint, so concatenating rows or adding counts cannot double-count a point
    for (const auto& sub_query : result.plan.sub_queries) {
        if (max_rows != 0 && result.count >= max_rows) break;

        request.box = sub_query;
        request.limit = max_rows == 0 ? 0 : max_rows - static_cast<uint32_t>(result.count);
        if (!coordinator.execute_filtered_query(request, result.records, result.count)) {
            // Rows from the shards that did answer would pass for a complete result
            result.records.clear();
            result.count = 0;
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

bool MortonClient::insert_batch(const std::vector<SpatialRecord>& records) {
    return coordinator.execute_2pc_transaction(coordinator.next_tx_id(), records);
}

SpatialQueryBuilder MortonClient::query() {
    return SpatialQueryBuilder(coordinator);
}
