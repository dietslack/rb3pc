// Logs where an unhandled guest access violation happened (src/crash_report.cpp).

#pragma once

namespace rb3pc {

// Call after setup (needs guest memory).
void InstallCrashReport();

}  // namespace rb3pc
