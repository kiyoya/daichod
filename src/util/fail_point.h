#pragma once

#include <cstdlib>
#include <cstring>

#include "rpc/error.h"

namespace daichod {

// Failure injection: if DAICHOD_FAIL_AT names this point, throw a ShimError
// instead of proceeding, simulating a recoverable engine failure (as opposed
// to DAICHOD_CRASH_AT / CrashPointMaybe, which abort()s with no cleanup to
// simulate a hard kill). The integration suite drives specific points and
// asserts callers handle the thrown error without wedging or crashing.
inline void FailPointMaybe(const char* name) {
  const char* target = std::getenv("DAICHOD_FAIL_AT");
  if (target != nullptr && std::strcmp(target, name) == 0) {
    throw ShimError(daicho::shim::v1::ERROR_CODE_ENGINE_ERROR,
                    "injected failure at fail point", name);
  }
}

}  // namespace daichod
