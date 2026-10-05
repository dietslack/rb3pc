// Overrides for RB3's App startup. Addresses are the retail XEX's (0.0.0.1);
// DX's executable has the same code elsewhere (src/exe.h).

#include <cstring>

#include <rex/hook.h>

#include "exe.h"

REX_EXTERN(RB3PC_MAIN_LOOP_FILTER);                // unhandled-exception filter that runs the main loop
REX_EXTERN(RB3PC_SET_UNHANDLED_EXCEPTION_FILTER);  // SetUnhandledExceptionFilter

// App::Run (0x82260430) installs 0x82260408 as the unhandled-exception filter
// and faults on purpose (`stw r10,0(0)`) so the filter runs
// App::RunWithoutDebugging (0x82260080). The runtime doesn't route guest
// faults, so dispatch here. 0x82C8B24C (previous filter) is left untouched,
// as the original only writes it after the fault.
REX_HOOK_RAW(RB3PC_APP_RUN) {
  const uint32_t app = __builtin_bswap32(ctx.r3.u32);
  std::memcpy(base + rb3pc::exe::kTheAppGlobal, &app, sizeof(app));  // App* read by the main loop

  ctx.r3.u64 = rb3pc::exe::kMainLoopFilter;
  RB3PC_SET_UNHANDLED_EXCEPTION_FILTER(ctx, base);

  ctx.r3.u64 = 0;  // EXCEPTION_POINTERS*, unused by the filter
  RB3PC_MAIN_LOOP_FILTER(ctx, base);
}
