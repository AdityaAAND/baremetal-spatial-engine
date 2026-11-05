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
    // Reads raw KITTI .bin files (x, y, z, reflectance) and streams points via MortonClient
    static KittiLoadResult load_bin_file(MortonClient& client, const std::string& filepath);
};