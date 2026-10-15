// Usage: xexdump <game_root> <xex name relative to game_root> <out_dir>
// Writes <out_dir>/<section>.bin per section plus sections.txt
// ("name vaddr size flags"), one line each.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <rex/kernel/init.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xex_module.h>

namespace fs = std::filesystem;
using rex::X_STATUS;

int main(int argc, char** argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: %s <game_root> <xex> <out_dir>\n", argv[0]);
    return 1;
  }
  fs::path out_dir = argv[3];
  fs::create_directories(out_dir);

  rex::Runtime runtime(fs::canonical(argv[1]).string());
  if (runtime.Setup(rex::RuntimeConfig{
          .kernel_init = rex::kernel::InitializeKernel,
          .tool_mode = true,
      }) != X_STATUS_SUCCESS) {
    std::fprintf(stderr, "runtime setup failed\n");
    return 1;
  }
  std::string vfs_path = std::string("game:\\") + argv[2];
  if (runtime.LoadXexImage(vfs_path) != X_STATUS_SUCCESS) {
    std::fprintf(stderr, "failed to load %s\n", vfs_path.c_str());
    return 1;
  }
  auto* xex = runtime.kernel_state()->GetExecutableModule()->xex_module();

  std::ofstream index(out_dir / "sections.txt");
  for (const auto& s : xex->binary_sections()) {
    std::ofstream bin(out_dir / (s.name.substr(s.name.find_first_not_of('.')) + ".bin"),
                      std::ios::binary);
    bin.write(reinterpret_cast<const char*>(s.host_data), s.virtual_size);
    char line[160];
    std::snprintf(line, sizeof(line), "%s 0x%08X 0x%08X %s%s\n", s.name.c_str(),
                  s.virtual_address, s.virtual_size, s.executable ? "x" : "-",
                  s.writable ? "w" : "-");
    index << line;
    std::fputs(line, stdout);
  }
  std::printf("base 0x%08X entry 0x%08X\n", xex->base_address(), xex->entry_point());
  if (const auto* exec = xex->opt_execution_info()) {
    auto v = exec->version();
    auto bv = rex::xex2_version{exec->base_version_value};
    std::printf("title %08X media %08X version %u.%u.%u.%u base %u.%u.%u.%u\n",
                uint32_t(exec->title_id), uint32_t(exec->media_id), v.major, v.minor,
                v.build, v.qfe, bv.major, bv.minor, bv.build, bv.qfe);
  }
  return 0;
}
