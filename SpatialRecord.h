#pragma once
#include<cstdint>
#pragma pack(push,1) // Force 1 byte  alignment (no padding)
struct SpatialRecord{
    uint64_t timestamp;//Epoch time
    float x;
    float y;
    float z;
    uint32_t metadata;
};
#pragma pack(pop)