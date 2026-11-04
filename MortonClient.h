#pragma once
#include <string>
#include <vector>
#include "SpatialRecord.h"
#include "NetworkProtocol.h"
#include "QueryOptimizer.h"
#include "Coordinator.h"

struct QueryResult {
    bool ok;                            // false means records is empty because the query failed
    std::vector<SpatialRecord> records;
    std::string error;
    ExecutionPlan plan;
};

// Borrows the Coordinator, which must outlive the builder and the client that made it
class SpatialQueryBuilder {
private:
    Coordinator& coordinator;
    QueryRequest req;
    bool bbox_set;

public:
    explicit SpatialQueryBuilder(Coordinator& coordinator_ref);
    SpatialQueryBuilder& bounding_box(float x_min, float x_max, float y_min, float y_max, float z_min, float z_max);
    QueryResult execute();
};

// Goes through the Coordinator so writes get routing and 2PC, and reads see every shard.
// Holds no connection between calls: workers serve one connection at a time, so a long-lived
// client would block every other client of that worker.
class MortonClient {
private:
    Coordinator& coordinator;

public:
    explicit MortonClient(Coordinator& coordinator_ref);

    // True only once the owning shard has committed the record through 2PC
    bool insert(const SpatialRecord& record);
    bool insert(float x, float y, float z, uint64_t timestamp = 0, uint32_t metadata = 1);
    
    SpatialQueryBuilder query();
};
