// Names the code behind an unhandled guest access violation. The SDK only
// logs the guest address, then resumes at the faulting instruction, which
// faults again forever: the game freezes and the log fills with the same line,
// rotating everything before it away. Recompiled functions call each other
// natively, so the host call stack is the game's: log it, symbolized from the
// executable's own symbol table, then end the process.

#include "crash_report.h"

#if defined(__linux__)

#include <cxxabi.h>
#include <dlfcn.h>
#include <elf.h>
#include <execinfo.h>
#include <link.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <rex/exception_handler.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/system/kernel_state.h>

namespace rb3pc {
namespace {

struct Symbol {
  uint64_t start, size;
  std::string name;
};

uintptr_t g_membase = 0;
// A fault nobody handles repeats at once, from the same instruction; give any
// handler installed after this one that many chances before calling it fatal.
constexpr int kFatalRepeats = 100;
std::atomic<uint64_t> g_last_pc{0}, g_last_fault{0};
std::atomic<int> g_repeats{0};

// Function symbols of /proc/self/exe, sorted by address (link-time offsets).
std::vector<Symbol> ReadSymbols() {
  std::vector<Symbol> out;
  std::ifstream file("/proc/self/exe", std::ios::binary);
  std::vector<char> data((std::istreambuf_iterator<char>(file)), {});
  if (data.size() < sizeof(Elf64_Ehdr)) return out;
  auto* eh = reinterpret_cast<const Elf64_Ehdr*>(data.data());
  if (std::memcmp(eh->e_ident, ELFMAG, SELFMAG) ||
      eh->e_shoff + uint64_t(eh->e_shnum) * sizeof(Elf64_Shdr) > data.size()) {
    return out;
  }
  auto* sh = reinterpret_cast<const Elf64_Shdr*>(data.data() + eh->e_shoff);
  for (int i = 0; i < eh->e_shnum; ++i) {
    if (sh[i].sh_type != SHT_SYMTAB || sh[i].sh_link >= eh->e_shnum) continue;
    const Elf64_Shdr& strtab = sh[sh[i].sh_link];
    if (sh[i].sh_offset + sh[i].sh_size > data.size() || strtab.sh_offset + strtab.sh_size > data.size()) continue;
    auto* syms = reinterpret_cast<const Elf64_Sym*>(data.data() + sh[i].sh_offset);
    for (size_t n = 0; n < sh[i].sh_size / sizeof(Elf64_Sym); ++n) {
      if (ELF64_ST_TYPE(syms[n].st_info) != STT_FUNC || !syms[n].st_value || syms[n].st_name >= strtab.sh_size) {
        continue;
      }
      out.push_back({syms[n].st_value, syms[n].st_size, data.data() + strtab.sh_offset + syms[n].st_name});
    }
  }
  std::sort(out.begin(), out.end(), [](const Symbol& a, const Symbol& b) { return a.start < b.start; });
  return out;
}

uintptr_t ExeLoadBias() {
  uintptr_t bias = 0;
  dl_iterate_phdr(
      [](dl_phdr_info* info, size_t, void* out) {
        *static_cast<uintptr_t*>(out) = info->dlpi_addr;
        return 1;  // the first entry is the executable
      },
      &bias);
  return bias;
}

std::string Demangle(const char* name) {
  int status = 0;
  char* demangled = abi::__cxa_demangle(name, nullptr, nullptr, &status);
  std::string out = status == 0 && demangled ? demangled : name;
  std::free(demangled);
  return out;
}

std::string Describe(const std::vector<Symbol>& symbols, uintptr_t bias, uintptr_t pc) {
  char offset_text[32];
  uint64_t offset = pc - bias;
  auto it = std::upper_bound(symbols.begin(), symbols.end(), offset,
                             [](uint64_t v, const Symbol& s) { return v < s.start; });
  if (it != symbols.begin() && offset < std::prev(it)->start + std::max<uint64_t>(std::prev(it)->size, 1)) {
    --it;
    std::snprintf(offset_text, sizeof(offset_text), "+0x%llX", static_cast<unsigned long long>(offset - it->start));
    return Demangle(it->name.c_str()) + offset_text;
  }
  // Shared libraries (the SDK runtime): exported names only.
  Dl_info info;
  if (dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_fname) {
    const char* file = std::strrchr(info.dli_fname, '/');
    file = file ? file + 1 : info.dli_fname;
    uintptr_t from = info.dli_sname ? uintptr_t(info.dli_saddr) : uintptr_t(info.dli_fbase);
    std::snprintf(offset_text, sizeof(offset_text), "+0x%llX", static_cast<unsigned long long>(pc - from));
    return std::string(file) + " " + (info.dli_sname ? Demangle(info.dli_sname) : "") + offset_text;
  }
  std::snprintf(offset_text, sizeof(offset_text), "0x%llX", static_cast<unsigned long long>(pc));
  return offset_text;
}

bool OnException(rex::arch::Exception* ex, void*) {
  if (ex->code() != rex::arch::Exception::Code::kAccessViolation) return false;
  uint64_t fault = ex->fault_address();
  if (fault < g_membase || fault >= g_membase + 0x100000000ull) return false;
  if (g_last_pc.exchange(ex->pc()) != ex->pc() || g_last_fault.exchange(fault) != fault) g_repeats = 0;
  if (++g_repeats < kFatalRepeats) return false;

  static const std::vector<Symbol> symbols = ReadSymbols();
  uintptr_t bias = ExeLoadBias();
  bool write = ex->access_violation_operation() == rex::arch::Exception::AccessViolationOperation::kWrite;
  REXLOG_ERROR("Crash: {} of guest 0x{:08X} in {}", write ? "write" : "read", uint32_t(fault - g_membase),
               Describe(symbols, bias, ex->pc()));
  void* frames[48];
  int count = backtrace(frames, 48);
  // Skip this handler and the signal frames: start at the faulting code.
  int first = 0;
  for (int i = 0; i < count; ++i) {
    if (reinterpret_cast<uintptr_t>(frames[i]) == ex->pc()) {
      first = i + 1;
      break;
    }
  }
  for (int i = first; i < count; ++i) {
    // Return addresses: step back into the call instruction.
    REXLOG_ERROR("Crash:   called from {}", Describe(symbols, bias, reinterpret_cast<uintptr_t>(frames[i]) - 1));
  }
  REXLOG_ERROR("Crash: the game can't continue past this; exiting.");
  rex::FlushLogging();
  std::_Exit(1);
}

}  // namespace

void InstallCrashReport() {
  g_membase = reinterpret_cast<uintptr_t>(rex::system::kernel_state()->memory()->virtual_membase());
  rex::arch::ExceptionHandler::Install(OnException, nullptr);
}

}  // namespace rb3pc

#else

namespace rb3pc {
void InstallCrashReport() {}
}  // namespace rb3pc

#endif
