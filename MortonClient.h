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
    uint64_t count;                     // matches found; the only result of execute_count
    std::string error;
    ExecutionPlan plan;
};

// Borrows the Coordinator, which must outlive the builder and the client that made it
class SpatialQueryBuilder {
private:
    Coordinator& coordinator;
    QueryRequest req;
    bool bbox_set;
    std::vector<QueryFilter> filters;
    uint32_t max_rows;

    QueryResult run(bool count_only);

public:
    explicit SpatialQueryBuilder(Coordinator& coordinator_ref);
    SpatialQueryBuilder& bounding_box(float x_min, float x_max, float y_min, float y_max, float z_min, float z_max);
    // The whole key space. Bounds beyond the quantizer's range also match records CMD_INGEST clamped.
    SpatialQueryBuilder& all();
    // Keeps records whose (metadata & mask) satisfies the comparison; several calls are ANDed, up to
    // MAX_QUERY_FILTERS. value must already sit in the mask's bits, e.g. dataset_id << 16 for DATASET_MASK.
    SpatialQueryBuilder& where(FilterOp op, uint32_t value, uint32_t mask = 0xFFFFFFFFu);
    SpatialQueryBuilder& limit(uint32_t rows); // 0 means no limit
    QueryResult execute();
    QueryResult execute_count(); // sets count without transferring any rows
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

    // One 2PC for the whole batch: all records commit or none do. Bulk loads need this because
    // each transaction costs a connection, and a closed connection holds its local port in TIME_WAIT.
    bool insert_batch(const std::vector<SpatialRecord>& records);
    
    SpatialQueryBuilder query();
};
