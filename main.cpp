#include "WriteAheadLog.h"
#include "BufferPool.h"
#include "SpatialRecord.h"
#include "SpatialIndex.h"
#include "Coordinator.h"
#include "WorkerServer.h"
#include "NetworkClient.h"
#include "MortonCode.h"
#include "MortonClient.h"
#include "QueryOptimizer.h"
#include <cmath>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>

static int failures = 0;

static void check(bool passed, const std::string& what) {
    std::cout << ">>> " << (passed ? "PASS" : "FAIL") << ": " << what << "\n";
    if (!passed) ++failures;
}

void run_worker(WorkerServer* server) {
    server->start_listening();
}

int main() {
    std::cout << "=================================================================\n";
    std::cout << "        MORTON-DB FULL CLUSTER VERIFICATION (LAYERS 4 AND 5)     \n";
    std::cout << "=================================================================\n";

    // 1. Provision Primary Worker 1 (Port 8081) and Standby Worker 2 (Port 8082)
    SpatialIndex primary_index;
    WriteAheadLog primary_wal("primary_wal.dat", 1024 * 1024 * 100);
    BufferPool primary_pool(1, primary_index);
    WorkerServer primary_worker(8081, primary_wal, primary_pool, primary_index);
    primary_worker.set_standby_node("127.0.0.1", 8082);
    std::thread t1(run_worker, &primary_worker);

    SpatialIndex standby_index;
    WriteAheadLog standby_wal("standby_wal.dat", 1024 * 1024 * 100);
    BufferPool standby_pool(1, standby_index);
    WorkerServer standby_worker(8082, standby_wal, standby_pool, standby_index);
    std::thread t2(run_worker, &standby_worker);

    // Second primary shard (Port 8083), also the target for the shard split test
    SpatialIndex shard2_index;
    WriteAheadLog shard2_wal("shard2_wal.dat", 1024 * 1024 * 100);
    BufferPool shard2_pool(1, shard2_index);
    WorkerServer shard2_worker(8083, shard2_wal, shard2_pool, shard2_index);
    std::thread t3(run_worker, &shard2_worker);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    Coordinator coordinator;
    coordinator.register_node(8081, {"127.0.0.1", 8081});
    coordinator.register_node(8082, {"127.0.0.1", 8082});
    coordinator.register_node(8083, {"127.0.0.1", 8083});

    // The top key bit is z's bit 20, so points with z < 48.576 land on 8081 and higher ones on 8083
    const uint64_t SHARD_BOUNDARY = 1ULL << 62;
    coordinator.register_shard(8081, 0, SHARD_BOUNDARY - 1);
    coordinator.register_shard(8083, SHARD_BOUNDARY, MAX_MORTON_KEY);

    // -------------------------------------------------------------------
    // STEP 0: SHARD-ROUTED 2PC TRANSACTIONS
    // -------------------------------------------------------------------
    std::cout << "\n--- TEST 0: Cross-Shard 2PC (each record goes only to its owning shard) ---\n";
    std::vector<SpatialRecord> good_batch = {
        {1700000000, 15.0f, 25.0f, 5.0f, 1},    // -> 8081
        {1700000001, 15.0f, 25.0f, 100.0f, 2}   // -> 8083
    };
    bool committed = coordinator.execute_2pc_transaction(9001, good_batch);
    check(committed && primary_index.size() >= 1 && shard2_index.size() >= 1, "TxID 9001 committed on both shards");

    size_t before_8081 = primary_index.size();
    size_t before_8083 = shard2_index.size();
    std::vector<SpatialRecord> bad_batch = {
        {1700000002, -1.0f, 25.0f, 5.0f, 3},    // -> 8081, rejected by validation (x < 0)
        {1700000003, 15.0f, 30.0f, 100.0f, 4}   // -> 8083, must not be stored either
    };
    bool aborted = !coordinator.execute_2pc_transaction(9002, bad_batch);
    check(aborted && primary_index.size() == before_8081 && shard2_index.size() == before_8083,
          "TxID 9002 aborted and left both shards unchanged");

    // -------------------------------------------------------------------
    // STEP 1: STREAM REPLICATION VERIFICATION
    // -------------------------------------------------------------------
    std::cout << "\n--- TEST 1: Primary -> Standby Real-Time WAL Streaming ---\n";
    NetworkClient client;
    bool ingested = false;
    if (client.connect_to_node("127.0.0.1", 8081)) {
        SpatialRecord rec1 = {1700000000, 15.0f, 25.0f, 5.0f, 1};
        ingested = client.send_record(rec1);
        client.close_connection();
    }
    check(ingested, "record ingested on primary 8081");

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // -------------------------------------------------------------------
    // STEP 2: SIMULATE PRIMARY CRASH & AUTOMATIC STANDBY FAILOVER
    // -------------------------------------------------------------------
    std::cout << "\n--- TEST 2: Primary Server Crash & Standby Failover ---\n";
    primary_worker.stop_server(); // Primary Node Crashes!
    
    // Coordinator detects missing socket/heartbeat and reroutes shard to Standby Node (8082)
    coordinator.failover_primary_to_standby(8081, 8082);

    // Query Standby Node directly to verify 0 data loss
    std::vector<SpatialRecord> matches;
    bool queried = client.connect_to_node("127.0.0.1", 8082)
                && client.send_query({10.0f, 20.0f, 20.0f, 30.0f, 0.0f, 10.0f}, matches);
    client.close_connection();
    check(queried && !matches.empty(), "standby 8082 holds the replicated record after failover");

    // -------------------------------------------------------------------
    // STEP 3: SHARD SPLIT
    // -------------------------------------------------------------------
    std::cout << "\n--- TEST 3: Dynamic Shard Split & Re-Sharding ---\n";
    // y = 90 sets key bit 61, which puts this record in the half of the range that migrates
    std::vector<SpatialRecord> split_batch = {{1700000004, 20.0f, 90.0f, 5.0f, 5}};
    check(coordinator.execute_2pc_transaction(9003, split_batch), "TxID 9003 committed on 8082 after failover");

    // After failover, the standby (8082) owns the old primary range; split it onto 8083
    bool split = coordinator.split_shard(8082, 8083, {"127.0.0.1", 8082}, {"127.0.0.1", 8083});
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    matches.clear();
    bool migrated = client.connect_to_node("127.0.0.1", 8083)
                 && client.send_query({19.0f, 21.0f, 89.0f, 91.0f, 4.0f, 6.0f}, matches);
    client.close_connection();
    check(split && migrated && matches.size() == 1, "split moved the upper half of the range to 8083");

    // -------------------------------------------------------------------
    // STEP 4: SDK AND OPTIMIZER, ON THE CLUSTER LEFT BY FAILOVER AND SPLIT
    // -------------------------------------------------------------------
    std::cout << "\n--- TEST 4: NoSQL SDK & Key-Range Optimizer ---\n";
    MortonClient sdk(coordinator);
    check(sdk.insert(12.5f, 25.0f, 8.0f, 1700000005, 100), "SDK insert committed on 8082");
    check(sdk.insert(45.0f, 90.0f, 15.0f, 1700000006, 200), "SDK insert committed on 8083");
    check(sdk.insert(15.0f, 25.0f, 48.5f, 1700000007, 300), "SDK insert just below z = 48.576");
    check(sdk.insert(15.0f, 25.0f, 48.6f, 1700000008, 400), "SDK insert just above z = 48.576");
    check(!sdk.insert(-1.0f, 25.0f, 5.0f, 1700000009, 500), "SDK insert with x < 0 rejected by 2PC validation");

    // No connection is held between calls, so a second client is not blocked by the first
    MortonClient second_sdk(coordinator);
    QueryResult narrow = second_sdk.query().bounding_box(12.0f, 13.0f, 20.0f, 30.0f, 5.0f, 10.0f).execute();
    std::cout << "[CBO Optimizer] " << narrow.plan.description << "\n";
    check(narrow.ok && narrow.records.size() == 1, "narrow query returned its one point");

    QueryResult cluster = sdk.query().bounding_box(0.0f, 50.0f, 0.0f, 100.0f, 0.0f, 120.0f).execute();
    std::cout << "[CBO Optimizer] " << cluster.plan.description << "\n";
    check(cluster.ok && cluster.records.size() == 7, "cluster-wide query returned all 7 points exactly once");

    QueryResult across = sdk.query().bounding_box(10.0f, 20.0f, 20.0f, 30.0f, 48.4f, 48.7f).execute();
    std::cout << "[CBO Optimizer] " << across.plan.description << "\n";
    check(across.ok && across.records.size() == 2 && across.plan.plan_type == ExecutionPlanType::MORTON_SPLIT_RANGE_SCAN,
          "box across the z boundary was split and found both points");

    // Aligned to the 2^18 mm Morton grid, so one range already covers exactly this cube
    ExecutionPlan aligned = QueryOptimizer::optimize({48.576f, 310.719f, 48.576f, 310.719f, 48.576f, 310.719f});
    std::cout << "[CBO Optimizer] " << aligned.description << "\n";
    check(aligned.plan_type == ExecutionPlanType::MORTON_INDEX_RANGE_SCAN, "grid-aligned cube stayed a single range");

    QueryResult inverted = sdk.query().bounding_box(20.0f, 10.0f, 20.0f, 30.0f, 5.0f, 10.0f).execute();
    check(!inverted.ok && inverted.plan.plan_type == ExecutionPlanType::INVALID_PLAN, "inverted box rejected");

    QueryResult not_a_number = sdk.query().bounding_box(NAN, 20.0f, 20.0f, 30.0f, 5.0f, 10.0f).execute();
    check(!not_a_number.ok && not_a_number.plan.plan_type == ExecutionPlanType::INVALID_PLAN, "NaN box rejected");

    check(!sdk.query().execute().ok, "query without a bounding box rejected");

    standby_worker.stop_server();
    shard2_worker.stop_server();
    if (t1.joinable()) t1.detach();
    if (t2.joinable()) t2.detach();
    if (t3.joinable()) t3.detach();

    std::cout << "\n=================================================================\n";
    if (failures == 0) {
        std::cout << "   ALL CHECKS PASSED: LAYERS 4 AND 5 VERIFIED                    \n";
    } else {
        std::cout << "   " << failures << " CHECK(S) FAILED                                               \n";
    }
    std::cout << "=================================================================\n";
    return failures == 0 ? 0 : 1;
}
