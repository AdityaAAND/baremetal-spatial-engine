
#include "KittiLoader.h"
#include <fstream>
#include <iostream>
#include <chrono>
#include <algorithm>

// Keeps a full frame (~115k points) to ~116 connections, far below the ephemeral port limit
static constexpr size_t INSERT_BATCH_SIZE = 1000;

KittiLoadResult KittiLoader::load_bin_file(MortonClient& client, const std::string& filepath, uint32_t dataset_id) {
    KittiLoadResult result{0, 0, 0.0, 0.0, false};

    std::ifstream input(filepath, std::ios::binary | std::ios::ate);
    if (!input.is_open()) {
        std::cerr << "[KittiLoader Error] Cannot open file: " << filepath << "\n";
        return result;
    }

    std::streamsize file_size = input.tellg();
    input.seekg(0, std::ios::beg);

    if (file_size <= 0 || file_size % (4 * sizeof(float)) != 0) {
        std::cerr << "[KittiLoader Error] Invalid binary file format (file size: " << file_size << " bytes).\n";
        return result;
    }

    size_t float_count = static_cast<size_t>(file_size) / sizeof(float);
    result.total_points_read = float_count / 4;

    std::vector<float> buffer(float_count);
    if (!input.read(reinterpret_cast<char*>(buffer.data()), file_size)) {
        std::cerr << "[KittiLoader Error] Failed to read binary buffer.\n";
        return result;
    }
    input.close();

    std::cout << "[KittiLoader] Read " << result.total_points_read 
              << " points (" << (file_size / (1024.0 * 1024.0)) << " MB) from " << filepath << "\n";

    auto start_time = std::chrono::high_resolution_clock::now();

    std::vector<SpatialRecord> batch;
    batch.reserve(INSERT_BATCH_SIZE);
    auto flush_batch = [&]() {
        if (batch.empty()) return;
        if (client.insert_batch(batch)) {
            result.points_inserted += batch.size();
        }
        batch.clear();
    };

    for (size_t i = 0; i < buffer.size(); i += 4) {
        float x = buffer[i];
        float y = buffer[i + 1];
        float z = buffer[i + 2];
        float reflectance = buffer[i + 3];

        const uint32_t intensity = static_cast<uint32_t>(std::clamp(reflectance * 255.0f, 0.0f, 255.0f));
        const uint32_t payload = pack_payload(dataset_id, intensity);

        batch.push_back({0, x, y, z, payload});
        if (batch.size() == INSERT_BATCH_SIZE) flush_batch();
    }
    flush_batch();

    auto end_time = std::chrono::high_resolution_clock::now();
    result.elapsed_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
    result.points_per_sec = (result.elapsed_ms > 0.0) 
        ? (static_cast<double>(result.points_inserted) / (result.elapsed_ms / 1000.0)) 
        : 0.0;
    result.success = (result.points_inserted > 0);

    std::cout << "[KittiLoader] Ingestion Summary:\n"
              << "  -> Points Ingested: " << result.points_inserted << " / " << result.total_points_read << "\n"
              << "  -> Time Elapsed:    " << result.elapsed_ms << " ms\n"
              << "  -> Throughput:      " << static_cast<uint64_t>(result.points_per_sec) << " pts/sec\n";

    return result;
}