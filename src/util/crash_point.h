#pragma once

#include <cstdlib>
#include <cstring>

namespace daichod {

// Crash-test injection: if DAICHOD_CRASH_AT names this point, die exactly
// here, with no cleanup — simulating a hard kill mid-protocol. The crash
// test suite drives every point and asserts the recovery handshake
// converges. Points: "after_intent", "after_apply", "after_outcome".
inline void CrashPointMaybe(const char* name) {
  const char* target = std::getenv("DAICHOD_CRASH_AT");
  if (target != nullptr && std::strcmp(target, name) == 0) std::abort();
}

}  // namespace daichod
