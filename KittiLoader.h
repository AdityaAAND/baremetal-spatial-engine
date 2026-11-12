#pragma once

#include <string>
#include <vector>
#include <cstddef>
#include <cstdint>
#include "MortonClient.h"

struct KittiLoadResult {
    size_t total_points_read;
    size_t points_inserted;
    double elapsed_ms;
    double points_per_sec;
    bool success;
};

class KittiLoader {
public:
    // Reads raw KITTI .bin files (x, y, z, reflectance) and streams points via MortonClient.
    // Each point's metadata is pack_payload(dataset_id, reflectance scaled to 0-255).
    static KittiLoadResult load_bin_file(MortonClient& client, const std::string& filepath, uint32_t dataset_id = 1);
};