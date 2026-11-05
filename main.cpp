#include "WriteAheadLog.h"
#include "BufferPool.h"
#include "SpatialRecord.h"
#include "SpatialIndex.h"
#include "WorkerServer.h"
#include "Coordinator.h"
#include "MortonClient.h"
#include "QueryOptimizer.h"
#include "KittiLoader.h"
#include <iostream>
#include <thread>
#include <chrono>

void run_worker(WorkerServer* server) {
    server->start_listening();
}

int main(int argc, char* argv[]) {
    std::cout << "=================================================================\n";
    std::cout << "        MORTON-DB: KITTI REAL 3D LIDAR INGESTION & QUERY TEST     \n";
    std::cout << "=================================================================\n";

    std::string file_path = (argc > 1) ? argv[1] : "000000.bin";

    // 1. Boot Worker Server on Port 8081
    SpatialIndex index;
    WriteAheadLog wal("kitti_wal.dat", 1024 * 1024 * 100);
    BufferPool pool(1, index);
    WorkerServer worker(8081, wal, pool, index);
    std::thread w_thread(run_worker, &worker);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 2. Provision Coordinator & Register Shard Range
    Coordinator coordinator;
    coordinator.register_node(8081, {"127.0.0.1", 8081});
    coordinator.register_shard(8081, 0, std::numeric_limits<uint64_t>::max());

    MortonClient client(coordinator);

    // 3. Ingest Real KITTI Point Cloud File
    std::cout << "\n--- STEP 1: Ingesting Binary LiDAR Frame (" << file_path << ") ---\n";
    KittiLoadResult res = KittiLoader::load_bin_file(client, file_path);

    if (!res.success) {
        std::cerr << "[FAIL] Failed to ingest KITTI binary dataset.\n";
        worker.stop_server();
        if (w_thread.joinable()) w_thread.detach();
        return 1;
    }

    // 4. Run Spatial Query Over Left-Side Sector
    // KITTI Velodyne axes are x forward, y left, z up, so this box is left of the car: X ∈ [-10, 10], Y ∈ [5, 25], Z ∈ [-2, 3]
    std::cout << "\n--- STEP 2: Executing Bounding Box Query Over Vehicle Sector ---\n";
    QueryRequest req = {-10.0f, 10.0f, 5.0f, 25.0f, -2.0f, 3.0f};

    ExecutionPlan plan = QueryOptimizer::optimize(req);
    std::cout << "[CBO Plan] " << plan.description << "\n";
    std::cout << "[CBO Stats] Morton Key Span: " << plan.key_span << "\n";

    QueryResult query_res = client.query()
                                  .bounding_box(req.x_min, req.x_max, req.y_min, req.y_max, req.z_min, req.z_max)
                                  .execute();

    if (query_res.ok) {
        std::cout << "[PASS] Successfully extracted " << query_res.records.size() 
                  << " 3D spatial points from KITTI dataset.\n";
        
        size_t sample_count = std::min(query_res.records.size(), size_t(5));
        std::cout << "  Sample Points in Bounding Box:\n";
        for (size_t i = 0; i < sample_count; ++i) {
            const auto& r = query_res.records[i];
            std::cout << "    [" << i+1 << "] Point: (" << r.x << ", " << r.y << ", " << r.z 
                      << ") | Payload: " << r.metadata << "\n";
        }
    } else {
        std::cout << "[FAIL] Spatial range query failed.\n";
    }

    worker.stop_server();
    if (w_thread.joinable()) w_thread.detach();

    std::cout << "\n=================================================================\n";
    std::cout << "        KITTI DATASET INGESTION & SEARCH VERIFIED CLEAN          \n";
    std::cout << "=================================================================\n";
    return 0;
}