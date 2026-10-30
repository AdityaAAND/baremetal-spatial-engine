#pragma once
#include <cstdint>

// Expands a 21-bit integer to 64 bits by inserting two zeros between every bit
uint64_t expand_bits(uint32_t v);

// Combines X, Y, and Z into a single 64-bit Z-order curve key
uint64_t calculate_z_order(uint32_t x, uint32_t y, uint32_t z);