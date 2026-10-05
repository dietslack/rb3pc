// Debug tracing of RB3's XAM profile/content calls (cvar trace_xam).
//
// Each traced import is overridden here, logs its arguments, then forwards to
// the SDK's implementation (looked up with dlsym(RTLD_NEXT)), and logs the
// result. Used to follow sign-in and save loading.

#include <dlfcn.h>

#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(trace_xam, false, "Debug",
                    "Log RB3's XAM user/content calls and their results");

namespace {

using GuestFn = void (*)(PPCContext&, uint8_t*);

GuestFn Next(const char* name) {
  auto fn = reinterpret_cast<GuestFn>(dlsym(RTLD_NEXT, name));
  if (!fn) REXLOG_ERROR("trace_xam: no SDK implementation for {}", name);
  return fn;
}

uint32_t BeU32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

std::string GuestString(uint8_t* base, uint32_t addr, size_t max = 64) {
  if (!addr) return "null";
  const char* s = reinterpret_cast<const char*>(base + addr);
  return std::string(s, strnlen(s, max));
}

// XCONTENT_DATA: u32 device_id, u32 content_type, u16 display_name[128],
// char file_name[42].
std::string ContentData(uint8_t* base, uint32_t addr) {
  if (!addr) return "null";
  const uint8_t* p = base + addr;
  std::string display;
  for (int i = 0; i < 128; ++i) {
    uint16_t c = uint16_t(p[8 + i * 2] << 8 | p[9 + i * 2]);
    if (!c) break;
    display += c < 128 ? char(c) : '?';
  }
  return fmt::format("{{device={:08X} type={:08X} name='{}' file='{}'}}", BeU32(p), BeU32(p + 4),
                     display, GuestString(base, addr + 8 + 256, 42));
}

}  // namespace

#define RB3PC_TRACE_XAM(name, describe_args)                                                  \
  REX_HOOK_RAW(__imp__##name) {                                                                \
    static GuestFn next = Next("__imp__" #name);                                               \
    if (!REXCVAR_GET(trace_xam)) {                                                             \
      next(ctx, base);                                                                         \
      return;                                                                                  \
    }                                                                                          \
    std::string args = describe_args;                                                          \
    next(ctx, base);                                                                           \
    REXLOG_INFO("XAM {}({}) -> {:08X}", #name, args, ctx.r3.u32);                              \
  }

#define R(n) ctx.r##n.u32

RB3PC_TRACE_XAM(XamContentCreateEx,
                fmt::format("user={} root='{}' data={} flags={:08X} disposition_out={:08X}",
                            R(3), GuestString(base, R(4)), ContentData(base, R(5)), R(6),
                            R(7)))
RB3PC_TRACE_XAM(XamContentCreateEnumerator,
                fmt::format("user={} device={:08X} type={:08X} flags={:08X} count={}", R(3), R(4),
                            R(5), R(6), R(7)))
RB3PC_TRACE_XAM(XamContentClose, fmt::format("root='{}'", GuestString(base, R(3))))
RB3PC_TRACE_XAM(XamContentFlush, fmt::format("root='{}'", GuestString(base, R(3))))
RB3PC_TRACE_XAM(XamContentDelete, fmt::format("user={} data={}", R(3), ContentData(base, R(4))))
RB3PC_TRACE_XAM(XamContentGetCreator,
                fmt::format("user={} data={}", R(3), ContentData(base, R(4))))
RB3PC_TRACE_XAM(XamContentGetDeviceState, fmt::format("device={:08X}", R(3)))
RB3PC_TRACE_XAM(XamContentGetDeviceData, fmt::format("device={:08X}", R(3)))
RB3PC_TRACE_XAM(XamShowDeviceSelectorUI,
                fmt::format("user={} type={:08X} flags={:08X}", R(3), R(4), R(5)))
RB3PC_TRACE_XAM(XamShowSigninUI, fmt::format("panes={} flags={:08X}", R(3), R(4)))
RB3PC_TRACE_XAM(XamUserGetSigninState, fmt::format("user={}", R(3)))
RB3PC_TRACE_XAM(XamUserGetSigninInfo, fmt::format("user={} flags={:08X}", R(3), R(4)))
RB3PC_TRACE_XAM(XamUserGetXUID, fmt::format("user={} type={:08X}", R(3), R(4)))
RB3PC_TRACE_XAM(XamUserGetName, fmt::format("user={}", R(3)))
RB3PC_TRACE_XAM(XamUserReadProfileSettings,
                fmt::format("title={:08X} user={} xuid_count={} setting_count={}", R(3), R(4),
                            R(5), R(7)))

#undef R
#undef RB3PC_TRACE_XAM
