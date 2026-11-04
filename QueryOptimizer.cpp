#include "QueryOptimizer.h"
#include "MortonCode.h"
#include <algorithm>
#include <cmath>

// Each sub-box costs a round trip per shard, which bounds how far a box is worth splitting
static constexpr size_t MAX_SUB_RANGES = 8;
// Splitting must cut the scanned key range by at least this factor to pay for those round trips
static constexpr uint64_t MIN_SPLIT_BENEFIT = 4;

static uint64_t key_span_of(const QueryRequest& box) {
    return point_key(box.x_max, box.y_max, box.z_max) - point_key(box.x_min, box.y_min, box.z_min);
}

// Largest float that quantizes below cut. Its successor is the smallest that reaches cut, so the
// two halves of a split share no point and leave no gap between them.
static float last_float_below(uint32_t cut) {
    float v = static_cast<float>(cut) / 1000.0f - COORD_OFFSET;
    while (quantize_coord(v) >= cut) v = std::nextafter(v, -INFINITY);
    while (quantize_coord(std::nextafter(v, INFINITY)) < cut) v = std::nextafter(v, INFINITY);
    return v;
}

// Splits at the highest bit where the corner keys differ, which is the jump that makes the
// Z-curve leave the box. Requires a non-zero key span.
static void split_box(const QueryRequest& box, QueryRequest& low, QueryRequest& high) {
    const uint64_t differing = point_key(box.x_min, box.y_min, box.z_min) ^ point_key(box.x_max, box.y_max, box.z_max);
    int bit = 63;
    while (!((differing >> bit) & 1)) --bit;

    const int axis = bit % 3; // x->bit0, y->bit1, z->bit2
    const int axis_bit = bit / 3;
    const float axis_max = axis == 0 ? box.x_max : axis == 1 ? box.y_max : box.z_max;
    const uint32_t cut = (quantize_coord(axis_max) >> axis_bit) << axis_bit;

    const float below = last_float_below(cut);
    const float above = std::nextafter(below, INFINITY);

    low = box;
    high = box;
    if (axis == 0)      { low.x_max = below; high.x_min = above; }
    else if (axis == 1) { low.y_max = below; high.y_min = above; }
    else                { low.z_max = below; high.z_min = above; }
}

ExecutionPlan QueryOptimizer::optimize(const QueryRequest& req) {
    ExecutionPlan plan{};
    plan.bounding_box = req;

    // Written as !(min <= max) so NaN bounds are rejected along with inverted ones
    if (!(req.x_min <= req.x_max) || !(req.y_min <= req.y_max) || !(req.z_min <= req.z_max)) {
        plan.plan_type = ExecutionPlanType::INVALID_PLAN;
        plan.description = "INVALID_PLAN: bounding box min must not exceed max";
        return plan;
    }

    plan.key_span = key_span_of(req);

    std::vector<QueryRequest> ranges = {req};
    while (ranges.size() < MAX_SUB_RANGES) {
        auto widest = std::max_element(ranges.begin(), ranges.end(), [](const QueryRequest& a, const QueryRequest& b) {
            return key_span_of(a) < key_span_of(b);
        });
        if (key_span_of(*widest) == 0) break;

        QueryRequest low, high;
        split_box(*widest, low, high);
        *widest = low;
        ranges.push_back(high);
    }

    uint64_t split_cost = 0;
    for (const auto& range : ranges) split_cost += key_span_of(range);

    // A scan on the ordered index never reads more than a full sweep would, so the real choice
    // is how many ranges to scan, not whether to use the index
    if (split_cost < plan.key_span / MIN_SPLIT_BENEFIT) {
        plan.plan_type = ExecutionPlanType::MORTON_SPLIT_RANGE_SCAN;
        plan.scan_cost = split_cost;
        plan.sub_queries = ranges;
        plan.description = "MORTON_SPLIT_RANGE_SCAN: " + std::to_string(ranges.size()) + " key ranges (key cost "
                         + std::to_string(split_cost) + " vs " + std::to_string(plan.key_span) + " for one range)";
    } else {
        plan.plan_type = ExecutionPlanType::MORTON_INDEX_RANGE_SCAN;
        plan.scan_cost = plan.key_span;
        plan.sub_queries = {req};
        plan.description = "MORTON_INDEX_RANGE_SCAN: 1 key range (key cost " + std::to_string(plan.key_span) + ")";
    }

    return plan;
}
