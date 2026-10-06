// SHA-1, for the game's file checksum table (see script_patches.cpp).

#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace rb3pc {

std::array<uint8_t, 20> Sha1(const std::vector<uint8_t>& data);

}  // namespace rb3pc
