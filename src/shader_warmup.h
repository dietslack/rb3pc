// Compiles RB3's shaders before the game starts, so a camera cut or venue
// that uses a new shader doesn't stall on the driver mid-song.

#pragma once

#include <filesystem>

namespace rb3pc {

// Hands the GPU backend every vertex/pixel shader pair in the game's shader
// caches (xbox_preinit_shaders, xbox_shaders in the ARK), read when shader
// storage loads. Off with --precompile_shaders=false.
void RegisterShaderWarmup(const std::filesystem::path& game_root);

}  // namespace rb3pc
