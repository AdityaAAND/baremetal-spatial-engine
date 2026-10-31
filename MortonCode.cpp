#include "MortonCode.h"

uint32_t quantize_coord(float v) {
    const float max_val = 2097151.0f; // 2^21 - 1
    float scaled = (v + COORD_OFFSET) * 1000.0f;
    if (!(scaled > 0.0f)) return 0; // also maps NaN to 0
    if (scaled > max_val) return 2097151u;
    return static_cast<uint32_t>(scaled);
}

uint64_t expand_bits(uint32_t v) {
    // Mask to 21 bits (since 21 bits * 3 coordinates = 63 bits, fitting in a 64-bit int)
    uint64_t x = v & 0x1FFFFF; 
    
    // Shift and mask to insert zeros between the original bits
    x = (x | x << 32) & 0x1F00000000FFFF;
    x = (x | x << 16) & 0x1F0000FF0000FF;
    x = (x | x << 8)  & 0x100F00F00F00F00F;
    x = (x | x << 4)  & 0x10C30C30C30C30C3;
    x = (x | x << 2)  & 0x1249249249249249;
    return x;
}

uint64_t calculate_z_order(uint32_t x, uint32_t y, uint32_t z) {
    // Interleave the bits: X gets bit 0, Y gets bit 1, Z gets bit 2
    return (expand_bits(x) | (expand_bits(y) << 1) | (expand_bits(z) << 2));
}