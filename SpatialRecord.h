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

// metadata layout: dataset id in the high 16 bits, intensity in the low 16. The id is on top so
// one dataset is a contiguous range of metadata values. Dataset 0 means "not from a LOAD".
constexpr uint32_t INTENSITY_MASK = 0x0000FFFFu;
constexpr uint32_t DATASET_MASK   = 0xFFFF0000u;
constexpr uint32_t MAX_DATASET_ID = 0xFFFFu;

constexpr uint32_t pack_payload(uint32_t dataset_id, uint32_t intensity) {
    return (dataset_id << 16) | (intensity & INTENSITY_MASK);
}
constexpr uint32_t payload_dataset(uint32_t metadata) { return metadata >> 16; }
constexpr uint32_t payload_intensity(uint32_t metadata) { return metadata & INTENSITY_MASK; }
