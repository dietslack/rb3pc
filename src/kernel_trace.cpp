// Debug timing of the kernel calls a guest thread can block in (cvar
// trace_kernel_time): file opens and reads, waits, sleeps. Each second, logs
// where each thread spent its time. Used to find what boot waits on.

#include <dlfcn.h>
#include <pthread.h>

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <algorithm>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "frame_stats.h"

REXCVAR_DEFINE_BOOL(trace_kernel_time, false, "Debug",
                    "Log, each second, the time guest threads spend in file I/O and waits");

namespace {

using GuestFn = void (*)(PPCContext&, uint8_t*);
using Clock = std::chrono::steady_clock;

struct Stat {
  uint64_t calls = 0, ns = 0, bytes = 0;
};

std::mutex g_mutex;
std::map<std::pair<std::string, std::string>, Stat> g_stats;  // (thread, call)

std::string ThreadName() {
  thread_local std::string name = [] {
    char buf[32] = {};
    pthread_getname_np(pthread_self(), buf, sizeof(buf));
    return std::string(buf);
  }();
  return name;
}

void Reporter() {
  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::vector<std::tuple<uint64_t, std::string, std::string, Stat>> rows;
    {
      std::lock_guard lock(g_mutex);
      for (auto& [key, stat] : g_stats) rows.emplace_back(stat.ns, key.first, key.second, stat);
      g_stats.clear();
    }
    std::sort(rows.begin(), rows.end(), [](auto& a, auto& b) { return std::get<0>(a) > std::get<0>(b); });
    std::string line;
    for (size_t i = 0; i < rows.size() && i < 20; ++i) {
      auto& [ns, thread, call, stat] = rows[i];
      if (ns < 20'000'000) break;  // under 20 ms
      line += fmt::format("  {}:{} {}x {}ms{}", thread, call, stat.calls, ns / 1'000'000,
                          stat.bytes ? fmt::format(" {}KB", stat.bytes / 1024) : "");
    }
    auto frames = rb3pc::GuestFrameStats();
    REXLOG_INFO("Kernel time: [{:.0f} fps, max {:.0f} ms]{}", frames.fps,
                rb3pc::GuestMaxFrameGapMs(), line);
  }
}

void Record(const char* call, uint64_t ns, uint64_t bytes) {
  static std::once_flag started;
  std::call_once(started, [] { std::thread(Reporter).detach(); });
  std::lock_guard lock(g_mutex);
  auto& s = g_stats[{ThreadName(), call}];
  ++s.calls;
  s.ns += ns;
  s.bytes += bytes;
}

GuestFn Next(const char* name) { return reinterpret_cast<GuestFn>(dlsym(RTLD_NEXT, name)); }

}  // namespace

namespace rb3pc {
// For time spent waiting outside kernel calls (e.g. gpu_wait.cpp).
void RecordWaitTime(const char* what, uint64_t ns) {
  if (REXCVAR_GET(trace_kernel_time)) Record(what, ns, 0);
}
}  // namespace rb3pc

// `bytes_reg`: register holding a byte count to total up, or -1.
#define RB3PC_TIME_KERNEL(name, bytes_expr)                                              \
  REX_HOOK_RAW(__imp__##name) {                                                          \
    static GuestFn next = Next("__imp__" #name);                                         \
    if (!REXCVAR_GET(trace_kernel_time)) {                                               \
      next(ctx, base);                                                                   \
      return;                                                                            \
    }                                                                                    \
    uint64_t bytes = bytes_expr;                                                         \
    auto start = Clock::now();                                                           \
    next(ctx, base);                                                                     \
    Record(#name,                                                                        \
           uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -  \
                                                                         start)          \
                        .count()),                                                       \
           bytes);                                                                       \
  }

RB3PC_TIME_KERNEL(NtReadFile, ctx.r9.u32)
RB3PC_TIME_KERNEL(NtReadFileScatter, ctx.r9.u32)
RB3PC_TIME_KERNEL(NtCreateFile, 0)
RB3PC_TIME_KERNEL(NtOpenFile, 0)
RB3PC_TIME_KERNEL(NtQueryFullAttributesFile, 0)
RB3PC_TIME_KERNEL(NtQueryInformationFile, 0)
RB3PC_TIME_KERNEL(KeWaitForSingleObject, 0)
RB3PC_TIME_KERNEL(KeWaitForMultipleObjects, 0)
RB3PC_TIME_KERNEL(NtWaitForSingleObjectEx, 0)
RB3PC_TIME_KERNEL(NtWaitForMultipleObjectsEx, 0)
RB3PC_TIME_KERNEL(KeDelayExecutionThread, 0)
RB3PC_TIME_KERNEL(RtlEnterCriticalSection, 0)

#undef RB3PC_TIME_KERNEL
