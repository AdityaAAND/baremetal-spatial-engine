#include "WriteAheadLog.h"
#include "BufferPool.h"
#include "SpatialRecord.h"
#include "SpatialIndex.h"
#include "Coordinator.h"
#include "WorkerServer.h"
#include "NetworkClient.h"
#include <iostream>
#include <thread>
#include <chrono>

void run_worker(WorkerServer* server) {
    server->start_listening();
}

int main() {
    // 1. Worker Node 1 Engine (Port 8081)
    SpatialIndex index1;
    WriteAheadLog wal1("worker1_wal.dat", 1024 * 1024 * 100);
    BufferPool pool1(3, index1);
    WorkerServer worker1(8081, wal1, pool1, index1);
    std::thread w1_thread(run_worker, &worker1);

    // 2. Worker Node 2 Engine (Port 8082)
    SpatialIndex index2;
    WriteAheadLog wal2("worker2_wal.dat", 1024 * 1024 * 100);
    BufferPool pool2(3, index2);
    WorkerServer worker2(8082, wal2, pool2, index2);
    std::thread w2_thread(run_worker, &worker2);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 3. Coordinator Setup
    Coordinator coordinator;
    std::vector<NodeEndpoint> participants = {
        {"127.0.0.1", 8081},
        {"127.0.0.1", 8082}
    };

    SpatialRecord cross_shard_point = {1700000000, 45.0f, 90.0f, 15.0f, 1};

    // 4. Execute Atomic 2PC Transaction
    uint64_t transaction_id = 9001;
    bool success = coordinator.execute_2pc_transaction(transaction_id, cross_shard_point, participants);

    worker1.stop_server();
    worker2.stop_server();

    if (w1_thread.joinable()) w1_thread.detach();
    if (w2_thread.joinable()) w2_thread.detach();

    std::cout << "Layer 4 2PC Consensus Execution Complete. Status: " << (success ? "COMMITTED" : "ABORTED") << "\n";
    return 0;
}