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
    // 1. Standby Worker Engine (Port 8082)
    SpatialIndex standby_index;
    WriteAheadLog standby_wal("standby_wal.dat", 1024 * 1024 * 100);
    BufferPool standby_pool(1, standby_index); // Flush immediately for testing
    WorkerServer standby_worker(8082, standby_wal, standby_pool, standby_index);
    std::thread standby_thread(run_worker, &standby_worker);

    // 2. Primary Worker Engine (Port 8081)
    SpatialIndex primary_index;
    WriteAheadLog primary_wal("primary_wal.dat", 1024 * 1024 * 100);
    BufferPool primary_pool(1, primary_index);
    WorkerServer primary_worker(8081, primary_wal, primary_pool, primary_index);
    primary_worker.set_standby_node("127.0.0.1", 8082); // Register Standby Target
    std::thread primary_thread(run_worker, &primary_worker);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 3. Client Ingests Point to Primary Node (8081)
    NetworkClient client;
    if (client.connect_to_node("127.0.0.1", 8081)) {
        SpatialRecord p1 = {1700000000, 25.0f, 35.0f, 10.0f, 1};
        std::cout << "[Client] Ingesting record (25, 35, 10) into Primary Worker (8081)...\n";
        client.send_record(p1);
        client.close_connection();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // 4. Client Queries Standby Node (8082) to Verify Replica State
    if (client.connect_to_node("127.0.0.1", 8082)) {
        std::cout << "\n[Client] Querying Standby Worker (8082) directly to verify data replication...\n";
        QueryRequest bounding_box = {20.0f, 30.0f, 30.0f, 40.0f, 5.0f, 15.0f};
        std::vector<SpatialRecord> replicated_records = client.send_query(bounding_box);

        std::cout << ">>> Standby Query Result: Found " << replicated_records.size() << " record(s) on backup node:\n";
        for (const auto& rec : replicated_records) {
            std::cout << "    Replicated Point: (" << rec.x << ", " << rec.y << ", " << rec.z << ")\n";
        }
        client.close_connection();
    }

    primary_worker.stop_server();
    standby_worker.stop_server();

    if (primary_thread.joinable()) primary_thread.detach();
    if (standby_thread.joinable()) standby_thread.detach();

    std::cout << "\nPrimary-Standby Streaming Replication Test Complete.\n";
    return 0;
}