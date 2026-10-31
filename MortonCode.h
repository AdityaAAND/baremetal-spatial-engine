#pragma once
#include <cstdint>

// Coordinates are shifted by COORD_OFFSET so negatives map to non-negative keys.
// Usable range is roughly [-COORD_OFFSET, -COORD_OFFSET + 2097.151]; values outside are clamped.
constexpr float COORD_OFFSET = 1000.0f;

// Order-preserving quantizer: monotone non-decreasing in v, millimeter resolution, clamped to 21 bits
uint32_t quantize_coord(float v);

// Expands a 21-bit integer to 64 bits by inserting two zeros between every bit
uint64_t expand_bits(uint32_t v);

// Combines X, Y, and Z into a single 64-bit Z-order curve key
uint64_t calculate_z_order(uint32_t x, uint32_t y, uint32_t z);