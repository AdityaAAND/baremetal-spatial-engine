#include "QueryOptimizer.h"
#include <cmath>
#include <algorithm>

ExecutionPlan QueryOptimizer::optimize(const QueryRequest& req) {
    ExecutionPlan plan;
    plan.bounding_box = req;

    float dx = std::abs(req.x_max - req.x_min);
    float dy = std::abs(req.y_max - req.y_min);
    float dz = std::abs(req.z_max - req.z_min);
    plan.calculated_volume = dx * dy * dz;

    uint32_t x1 = quantize_coord(req.x_min); uint32_t y1 = quantize_coord(req.y_min); uint32_t z1 = quantize_coord(req.z_min);
    uint32_t x2 = quantize_coord(req.x_max); uint32_t y2 = quantize_coord(req.y_max); uint32_t z2 = quantize_coord(req.z_max);

    uint64_t min_key = calculate_z_order(x1, y1, z1);
    uint64_t max_key = calculate_z_order(x2, y2, z2);
    if (min_key > max_key) std::swap(min_key, max_key);
    plan.key_span = max_key - min_key;

    // Cost Threshold: Queries covering under 1,000,000 m^3 use 64-bit Morton index range scan
    if (plan.calculated_volume < 1000000.0f) {
        plan.plan_type = ExecutionPlanType::MORTON_INDEX_RANGE_SCAN;
        plan.description = "MORTON_INDEX_RANGE_SCAN: Selective 3D Volume (" + std::to_string(plan.calculated_volume) + " m^3)";
    } else {
        plan.plan_type = ExecutionPlanType::FULL_SHARD_SWEEP;
        plan.description = "FULL_SHARD_SWEEP: Broad 3D Volume (" + std::to_string(plan.calculated_volume) + " m^3)";
    }

    return plan;
}
