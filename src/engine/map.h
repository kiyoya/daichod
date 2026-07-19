#pragma once

#include <string>

#include <guid.h>

namespace daichod {

// proto <-> engine primitive conversions. Entity mapping (Account,
// Transaction, ...) accretes here as the services land.

// Formats an engine GUID as the 32-hex-char string the contract uses.
std::string GuidToString(const GncGUID* guid);

// Parses a contract GUID string; throws ShimError{INVALID_ARGUMENT_DETAIL}
// on malformed input. `context` names the offending field for ErrorDetail.
GncGUID StringToGuid(const std::string& text, const std::string& context);

// Strips any password from a backend URI (postgres://user:pw@host/db) so
// GetBookInfo never echoes credentials.
std::string RedactUri(const std::string& uri);

}  // namespace daichod
