#include "engine/map.h"

#include "rpc/error.h"

namespace daichod {

std::string GuidToString(const GncGUID* guid) {
  char buffer[GUID_ENCODING_LENGTH + 1];
  guid_to_string_buff(guid, buffer);
  return std::string(buffer);
}

GncGUID StringToGuid(const std::string& text, const std::string& context) {
  GncGUID guid;
  if (!string_to_guid(text.c_str(), &guid)) {
    throw ShimError(daicho::shim::v1::INVALID_ARGUMENT_DETAIL,
                    "malformed GUID: " + text, context);
  }
  return guid;
}

std::string RedactUri(const std::string& uri) {
  // scheme://user:password@rest -> scheme://user:***@rest
  const std::string::size_type scheme_end = uri.find("://");
  if (scheme_end == std::string::npos) return uri;
  const std::string::size_type at = uri.find('@', scheme_end + 3);
  if (at == std::string::npos) return uri;
  const std::string::size_type colon = uri.find(':', scheme_end + 3);
  if (colon == std::string::npos || colon > at) return uri;
  return uri.substr(0, colon + 1) + "***" + uri.substr(at);
}

}  // namespace daichod
