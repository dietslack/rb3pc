// Addresses that differ between retail default.xex (0.0.0.1, rb3pc) and
// Deluxe's (0.0.5.1, TU5-based, rb3pc-dx / RB3PC_DX). Mid-asm hooks live in
// config/{retail,dx}/hooks.toml. DX addresses found by matching retail code.

#pragma once

#include <cstdint>

#ifdef RB3PC_DX

// App::Run, the exception filter that runs the main loop, and
// SetUnhandledExceptionFilter (src/app_hooks.cpp).
#define RB3PC_APP_RUN sub_822703D0
#define RB3PC_MAIN_LOOP_FILTER sub_822703A8
#define RB3PC_SET_UNHANDLED_EXCEPTION_FILTER sub_8283C6B0

namespace rb3pc::exe {
constexpr bool kDeluxe = true;
constexpr uint32_t kTheAppGlobal = 0x82CBC600;
constexpr uint32_t kMainLoopFilter = 0x822703A8;
constexpr uint32_t kRdata = 0x82000400, kRdataSize = 0x001F1184;
constexpr uint32_t kData = 0x82C64400, kDataSize = 0x001F5EAC;
}  // namespace rb3pc::exe

#else

#define RB3PC_APP_RUN sub_82260430
#define RB3PC_MAIN_LOOP_FILTER sub_82260408
#define RB3PC_SET_UNHANDLED_EXCEPTION_FILTER sub_82815A10

namespace rb3pc::exe {
constexpr bool kDeluxe = false;
constexpr uint32_t kTheAppGlobal = 0x82C8B240;
constexpr uint32_t kMainLoopFilter = 0x82260408;
constexpr uint32_t kRdata = 0x82000400, kRdataSize = 0x001E95AC;
constexpr uint32_t kData = 0x82C34400, kDataSize = 0x001F35EC;
}  // namespace rb3pc::exe

#endif
