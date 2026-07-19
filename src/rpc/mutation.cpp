#include "rpc/mutation.h"

#include <cctype>

namespace daichod {

void ValidateMutationId(const std::string& mutation_id) {
  static constexpr char kPattern[] = "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx";
  bool valid = mutation_id.size() == sizeof(kPattern) - 1;
  if (valid) {
    for (size_t i = 0; i < mutation_id.size(); ++i) {
      if (kPattern[i] == '-') {
        if (mutation_id[i] != '-') valid = false;
      } else if (std::isxdigit(static_cast<unsigned char>(mutation_id[i])) ==
                 0) {
        valid = false;
      }
    }
  }
  if (!valid) {
    throw ShimError(daicho::shim::v1::ERROR_CODE_INVALID_ARGUMENT,
                    "mutation_id must be a UUID", mutation_id);
  }
}

}  // namespace daichod
