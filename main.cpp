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
    // 1. Initialize Worker Local Engine (Layer 1 & 2)
    SpatialIndex worker_index;
    WriteAheadLog worker_wal("worker_wal.dat", 1024 * 1024 * 100);
    BufferPool worker_pool(3, worker_index);

    // 2. Start Worker Server on port 8081 in background thread
    WorkerServer worker_server(8081, worker_wal, worker_pool);
    std::thread worker_thread(run_worker, &worker_server);

    // Give the server thread a moment to bind to the socket
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 3. Coordinator connects via Network Client
    Coordinator coordinator;
    NetworkClient client;

    std::cout << "[Coordinator] Connecting to Worker Node at 127.0.0.1:8081...\n";
    if (client.connect_to_node("127.0.0.1", 8081)) {
        SpatialRecord radar_point = {1700000000, 12.4f, 45.2f, 8.1f, 1};
        
        std::cout << "[Coordinator] Streaming coordinate over TCP socket...\n";
        client.send_record(radar_point);
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        client.close_connection();
    }

    worker_server.stop_server();
    if (worker_thread.joinable()) worker_thread.join();

    std::cout << "Distributed network pipeline execution complete.\n";
    return 0;
}