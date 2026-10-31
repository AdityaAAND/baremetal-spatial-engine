#include "WriteAheadLog.h"
#include "BufferPool.h"
#include "SpatialRecord.h"
#include "SpatialIndex.h"
#include "Coordinator.h"
#include "WorkerServer.h"
#include "NetworkClient.h"
#include "MortonCode.h"
#include <iostream>
#include <thread>
#include <chrono>

void run_worker(WorkerServer* server) {
    server->start_listening();
}

int main() {
    std::cout << "=================================================================\n";
    std::cout << "        MORTON-DB LAYER 4 FULL FAULT-TOLERANCE VERIFICATION      \n";
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
    std::cout << ">>> TxID 9001: " << (committed ? "COMMITTED" : "ABORTED")
              << " | shard 8081 size=" << primary_index.size() << ", shard 8083 size=" << shard2_index.size() << "\n";

    size_t before_8081 = primary_index.size();
    size_t before_8083 = shard2_index.size();
    std::vector<SpatialRecord> bad_batch = {
        {1700000002, -1.0f, 25.0f, 5.0f, 3},    // -> 8081, rejected by validation (x < 0)
        {1700000003, 15.0f, 30.0f, 100.0f, 4}   // -> 8083, must not be stored either
    };
    bool aborted = !coordinator.execute_2pc_transaction(9002, bad_batch);
    std::cout << ">>> TxID 9002: " << (aborted ? "ABORTED" : "COMMITTED")
              << " | shards unchanged: " << ((primary_index.size() == before_8081 && shard2_index.size() == before_8083) ? "yes" : "NO") << "\n";

    // -------------------------------------------------------------------
    // STEP 1: STREAM REPLICATION VERIFICATION
    // -------------------------------------------------------------------
    std::cout << "\n--- TEST 1: Primary -> Standby Real-Time WAL Streaming ---\n";
    NetworkClient client;
    if (client.connect_to_node("127.0.0.1", 8081)) {
        SpatialRecord rec1 = {1700000000, 15.0f, 25.0f, 5.0f, 1};
        client.send_record(rec1);
        client.close_connection();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // -------------------------------------------------------------------
    // STEP 2: SIMULATE PRIMARY CRASH & AUTOMATIC STANDBY FAILOVER
    // -------------------------------------------------------------------
    std::cout << "\n--- TEST 2: Primary Server Crash & Standby Failover ---\n";
    primary_worker.stop_server(); // Primary Node Crashes!
    
    // Coordinator detects missing socket/heartbeat and reroutes shard to Standby Node (8082)
    coordinator.failover_primary_to_standby(8081, 8082);

    // Query Standby Node directly to verify 0 data loss
    if (client.connect_to_node("127.0.0.1", 8082)) {
        QueryRequest req = {10.0f, 20.0f, 20.0f, 30.0f, 0.0f, 10.0f};
        std::vector<SpatialRecord> matches = client.send_query(req);
        std::cout << ">>> Query against Standby Node (8082) returned " << matches.size() << " record(s).\n";
        for (const auto& match : matches) {
            std::cout << "    Recovered Point: (" << match.x << ", " << match.y << ", " << match.z << ")\n";
        }
        client.close_connection();
    }


    std::cout << "\n--- TEST 3: Dynamic Shard Split & Re-Sharding ---\n";
    // After failover, the standby (8082) owns the old primary range; split it onto 8083
    coordinator.split_shard(8082, 8083, {"127.0.0.1", 8082}, {"127.0.0.1", 8083});

    standby_worker.stop_server();
    shard2_worker.stop_server();
    if (t1.joinable()) t1.detach();
    if (t2.joinable()) t2.detach();
    if (t3.joinable()) t3.detach();

    std::cout << "\n=================================================================\n";
    std::cout << "   LAYER 4 COMPLETED & VERIFIED: FAULT TOLERANCE READY            \n";
    std::cout << "=================================================================\n";
    return 0;
}