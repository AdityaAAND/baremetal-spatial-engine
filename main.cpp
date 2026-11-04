#include "WriteAheadLog.h"
#include "BufferPool.h"
#include "SpatialRecord.h"
#include "SpatialIndex.h"
#include "WorkerServer.h"
#include "MortonClient.h"
#include "QueryOptimizer.h"
#include <iostream>
#include <thread>
#include <chrono>

void run_worker(WorkerServer* server) {
    server->start_listening();
}

int main() {
    std::cout << "=================================================================\n";
    std::cout << "      MORTON-DB LAYER 5: NoSQL SDK & KEY-RANGE OPTIMIZER TEST    \n";
    std::cout << "=================================================================\n";

    // 1. Launch Worker Server on Port 8081
    SpatialIndex index;
    WriteAheadLog wal("nosql_layer5_wal.dat", 1024 * 1024 * 100);
    BufferPool pool(1, index);
    WorkerServer worker(8081, wal, pool, index);
    std::thread w_thread(run_worker, &worker);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 2. Native NoSQL C++ SDK Execution
    MortonClient client("127.0.0.1", 8081);
    if (client.connect()) {
        std::cout << "\n[NoSQL SDK] Connected to Worker Server. Ingesting points...\n";
        client.insert(12.5f, 25.0f, 8.0f, 1700000000, 100);
        client.insert(45.0f, 90.0f, 15.0f, 1700000001, 200);

        // 3. Construct NoSQL Bounding-Box Query Request
        QueryRequest req = {10.0f, 20.0f, 20.0f, 30.0f, 5.0f, 10.0f};

        // 4. Pass Request Through Cost-Based Optimizer
        ExecutionPlan plan = QueryOptimizer::optimize(req);
        std::cout << "[CBO Optimizer] Plan Chosen: " << plan.description << "\n";
        std::cout << "[CBO Optimizer] Calculated Volume: " << plan.calculated_volume 
                  << " m^3 | Morton Key Span: " << plan.key_span << "\n";

        // 5. Execute Optimized Query via Fluent SDK
        auto results = client.query()
                             .bounding_box(req.x_min, req.x_max, req.y_min, req.y_max, req.z_min, req.z_max)
                             .execute();

        std::cout << "\n[NoSQL SDK] Returned " << results.size() << " point(s):\n";
        for (const auto& rec : results) {
            std::cout << "            Point: (" << rec.x << ", " << rec.y << ", " << rec.z << ")\n";
        }

        client.disconnect();
    }

    worker.stop_server();
    if (w_thread.joinable()) w_thread.detach();

    std::cout << "\n=================================================================\n";
    std::cout << "               LAYER 5 NoSQL SDK ENGINE VERIFIED                 \n";
    std::cout << "=================================================================\n";
    return 0;
}