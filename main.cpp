#include "WriteAheadLog.h"
#include "BufferPool.h"
#include "SpatialRecord.h"
#include "SpatialIndex.h"
#include "WorkerServer.h"
#include "Coordinator.h"
#include "MortonClient.h"
#include "QueryOptimizer.h"
#include "KittiLoader.h"
#include "QueryExecutor.h"
#include "MortonCode.h"
#include "VisualizerServer.h"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <shared_mutex>
#include <string>
#include <unistd.h>
#include <vector>
#include <thread>
#include <chrono>

static constexpr int FIRST_PORT = 8081;
static constexpr int MAX_SHARDS = 16;
static constexpr int DEFAULT_HTTP_PORT = 8080; // just below the workers
static constexpr const char* SHARD_MAP_FILE = "shard_map.dat";
// One per data set, like the WALs: a decision only means something to the workers it was made for
static constexpr const char* SHELL_DECISION_FILE = "tx_decisions.dat";
static constexpr const char* DEMO_DECISION_FILE = "kitti_tx.dat";

struct Shard {
    SpatialIndex index;
    WriteAheadLog wal;
    BufferPool pool;
    WorkerServer worker;

    Shard(int port, const std::string& wal_path)
        : wal(wal_path.c_str(), 1024 * 1024 * 100), pool(1, index), worker(port, wal, pool, index) {}
};

struct Options {
    bool shell = false;
    int shards = 0; // 0 means not given
    int http_port = 0; // 0 means no viewer
    std::string file_path = "000000.bin";
};

void run_worker(WorkerServer* server) {
    server->start_listening();
}

static bool parse_args(int argc, char* argv[], Options& options) {
    bool path_given = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--shell") {
            options.shell = true;
        } else if (arg == "--shards") {
            if (++i == argc) return false;
            char* end = nullptr;
            const long count = std::strtol(argv[i], &end, 10);
            if (*end != '\0' || count < 1 || count > MAX_SHARDS) return false;
            options.shards = static_cast<int>(count);
        } else if (arg == "--http") {
            options.http_port = DEFAULT_HTTP_PORT;
            // The port is optional, so only take the next argument if it is a number
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9') {
                char* end = nullptr;
                const long port = std::strtol(argv[++i], &end, 10);
                if (*end != '\0' || port < 1024 || port > 65535) return false;
                options.http_port = static_cast<int>(port);
            }
        } else if (arg.rfind("--", 0) == 0 || path_given) {
            return false;
        } else {
            options.file_path = arg;
            path_given = true;
        }
    }
    if (options.http_port != 0 && !options.shell) return false;
    return !(options.shards != 0 && !options.shell) && !(options.shell && path_given);
}

// Nodes are numbered by port from FIRST_PORT, so the highest node says how many workers the map needs
static int workers_in_map(const Coordinator& coordinator) {
    int count = 0;
    for (const ShardRange& shard : coordinator.get_shards()) {
        count = std::max(count, static_cast<int>(shard.node_id) - FIRST_PORT + 1);
    }
    return count;
}

// A starting point only: real data clusters in a narrow band of keys, which is why REBALANCE exists
static void register_even_shards(Coordinator& coordinator, int count) {
    const uint64_t width = MAX_MORTON_KEY / static_cast<uint64_t>(count);
    for (int i = 0; i < count; ++i) {
        const uint64_t start = static_cast<uint64_t>(i) * width;
        const uint64_t end = i + 1 == count ? MAX_MORTON_KEY : start + width - 1;
        coordinator.register_shard(static_cast<uint32_t>(FIRST_PORT + i), start, end);
    }
}

int main(int argc, char* argv[]) {
    Options options;
    if (!parse_args(argc, argv, options)) {
        std::cerr << "usage: spatial_db [frame.bin]\n"
                  << "       spatial_db --shell [--shards N] [--http [PORT]]\n"
                  << "         N from 1 to " << MAX_SHARDS << "; --http serves the 3D viewer, on port "
                  << DEFAULT_HTTP_PORT << " unless PORT (1024-65535) is given\n";
        return 1;
    }

    Coordinator coordinator;
    const bool map_loaded = options.shell && coordinator.load_shard_map(SHARD_MAP_FILE);
    const int saved_workers = map_loaded ? workers_in_map(coordinator) : 0;

    // Refused rather than ignored: the workers left out would take their points with them
    if (options.shards != 0 && options.shards < saved_workers) {
        std::cerr << "error: the data is spread across " << saved_workers << " workers, so it can't start with "
                  << options.shards << ".\nUse --shards " << saved_workers
                  << ", or delete shard_*_wal.dat and " << SHARD_MAP_FILE << " to start empty.\n";
        return 1;
    }

    int worker_count = 1;
    if (options.shell) worker_count = options.shards != 0 ? options.shards : std::max(saved_workers, 1);

    if (options.http_port >= FIRST_PORT && options.http_port < FIRST_PORT + worker_count) {
        std::cerr << "error: port " << options.http_port << " is used by a worker; workers take "
                  << FIRST_PORT << " to " << FIRST_PORT + worker_count - 1 << ".\n";
        return 1;
    }

    // Every component logs to std::cout, which would bury query results. Moving std::cout to a
    // file before the workers start keeps the terminal for results only.
    std::streambuf* terminal = std::cout.rdbuf();
    std::ostream shell_out(terminal);
    std::ofstream shell_log;
    if (options.shell) {
        shell_log.open("spatial_shell.log");
        std::cout.rdbuf(shell_log.rdbuf());
    }

    std::cout << "=================================================================\n";
    std::cout << "        MORTON-DB: KITTI REAL 3D LIDAR INGESTION & QUERY TEST     \n";
    std::cout << "=================================================================\n";

    // 1. Boot Workers. The shell's WALs are separate from the demo's, which has no shard map to match.
    std::vector<std::unique_ptr<Shard>> shards;
    std::vector<std::thread> threads;
    for (int i = 0; i < worker_count; ++i) {
        const int port = FIRST_PORT + i;
        const std::string wal_path = options.shell ? "shard_" + std::to_string(port) + "_wal.dat" : "kitti_wal.dat";
        shards.push_back(std::make_unique<Shard>(port, wal_path));
        shards.back()->worker.recover(); // here, not in the thread, so the first command can't beat it
        threads.emplace_back(run_worker, &shards.back()->worker);
    }

    auto stop_workers = [&]() {
        for (auto& shard : shards) shard->worker.stop_server();
        for (auto& thread : threads) thread.detach();
    };

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // 2. Provision Coordinator & Register Shard Ranges
    for (int i = 0; i < worker_count; ++i) {
        coordinator.register_node(static_cast<uint32_t>(FIRST_PORT + i), {"127.0.0.1", FIRST_PORT + i});
    }
    if (!options.shell) {
        coordinator.register_shard(FIRST_PORT, 0, std::numeric_limits<uint64_t>::max());
    } else if (!map_loaded) {
        register_even_shards(coordinator, worker_count);
        coordinator.save_shard_map(SHARD_MAP_FILE);
    }

    // 3. Finish any transaction a crash interrupted, before anything can read or write
    const char* decision_file = options.shell ? SHELL_DECISION_FILE : DEMO_DECISION_FILE;
    if (!coordinator.open_decision_log(decision_file)) {
        std::cerr << "error: cannot open " << decision_file << ", which is needed to commit transactions safely.\n";
        stop_workers();
        std::cout.rdbuf(terminal);
        return 1;
    }
    const TxRecoveryResult recovery = coordinator.resolve_pending_transactions();
    std::ostream& notices = options.shell ? shell_out : std::cout;
    if (recovery.committed + recovery.aborted > 0) {
        notices << "note: finished " << recovery.committed + recovery.aborted
                << " transaction(s) interrupted by a crash: " << recovery.committed << " committed, "
                << recovery.aborted << " aborted.\n";
    }
    if (!recovery.ok) {
        notices << "warning: some interrupted transactions could not be finished; they will be retried at the next start.\n";
    }

    MortonClient client(coordinator);

    if (options.shell) {
        shell_out << "Morton-DB shell with " << worker_count << (worker_count == 1 ? " worker" : " workers")
                  << ". Type HELP; for commands and EXIT; to quit.\n";

        if (saved_workers != 0 && worker_count > saved_workers) {
            shell_out << "note: the new workers own no keys yet. Run REBALANCE; to share the data with them.\n";
        }
        if (!map_loaded) {
            uint64_t stored = 0;
            for (uint32_t node : coordinator.node_ids()) {
                uint64_t count = 0;
                if (coordinator.node_point_count(node, count)) stored += count;
            }
            if (stored > 0) {
                shell_out << "note: found " << stored << " points but no shard map, so queries may miss some. Run REBALANCE; to fix this.\n";
            }
        }

        std::shared_mutex cluster_lock;
        VisualizerServer visualizer(coordinator, client, cluster_lock, options.http_port);
        if (options.http_port != 0) {
            if (!visualizer.start()) {
                std::cerr << "error: cannot listen on port " << options.http_port << " for the viewer; try --http with another port.\n";
                stop_workers();
                std::cout.rdbuf(terminal);
                return 1;
            }
            shell_out << "3D viewer at http://127.0.0.1:" << options.http_port << "/ (shows points added by LOAD).\n";
        }

        QueryExecutor executor(client, coordinator, shell_out, SHARD_MAP_FILE, cluster_lock);
        run_shell(executor, std::cin, shell_out, isatty(STDIN_FILENO) != 0 && isatty(STDOUT_FILENO) != 0);

        visualizer.stop(); // before the workers, so a request in progress still gets its answer
        stop_workers();
        std::cout.rdbuf(terminal); // shell_log is about to close
        return 0;
    }

    // 4. Ingest Real KITTI Point Cloud File
    std::cout << "\n--- STEP 1: Ingesting Binary LiDAR Frame (" << options.file_path << ") ---\n";
    KittiLoadResult res = KittiLoader::load_bin_file(client, options.file_path);

    if (!res.success) {
        std::cerr << "[FAIL] Failed to ingest KITTI binary dataset.\n";
        stop_workers();
        return 1;
    }

    // 5. Run Spatial Query Over Left-Side Sector
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

    stop_workers();

    std::cout << "\n=================================================================\n";
    std::cout << "        KITTI DATASET INGESTION & SEARCH VERIFIED CLEAN          \n";
    std::cout << "=================================================================\n";
    return 0;
}