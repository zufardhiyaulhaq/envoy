#include "extensions/filters/http/ext_proc/mutation_utils.h"

#include "envoy/http/header_map.h"

#include "common/http/header_utility.h"
#include "common/http/headers.h"
#include "common/protobuf/utility.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace ExternalProcessing {

using Http::Headers;
using Http::LowerCaseString;

using envoy::service::ext_proc::v3alpha::BodyMutation;
using envoy::service::ext_proc::v3alpha::BodyResponse;
using envoy::service::ext_proc::v3alpha::HeaderMutation;
using envoy::service::ext_proc::v3alpha::HeadersResponse;

void MutationUtils::buildHttpHeaders(const Http::HeaderMap& headers_in,
                                     envoy::config::core::v3::HeaderMap& headers_out) {
  headers_in.iterate([&headers_out](const Http::HeaderEntry& e) -> Http::HeaderMap::Iterate {
    auto* new_header = headers_out.add_headers();
    new_header->set_key(std::string(e.key().getStringView()));
    new_header->set_value(std::string(e.value().getStringView()));
    return Http::HeaderMap::Iterate::Continue;
  });
}

void MutationUtils::applyCommonHeaderResponse(const HeadersResponse& response,
                                              Http::HeaderMap& headers) {
  if (response.has_response()) {
    const auto& common_response = response.response();
    if (common_response.has_header_mutation()) {
      applyHeaderMutations(common_response.header_mutation(), headers);
    }
  }
}

void MutationUtils::applyHeaderMutations(const HeaderMutation& mutation, Http::HeaderMap& headers) {
  for (const auto& remove_header : mutation.remove_headers()) {
    if (Http::HeaderUtility::isRemovableHeader(remove_header)) {
      headers.remove(LowerCaseString(remove_header));
    }
  }

  for (const auto& sh : mutation.set_headers()) {
    if (!sh.has_header()) {
      continue;
    }
    if (isSettableHeader(sh.header().key())) {
      // Make "false" the default. This is logical and matches the ext_authz
      // filter. However, the router handles this same protobuf and uses "true"
      // as the default instead.
      const bool append = PROTOBUF_GET_WRAPPED_OR_DEFAULT(sh, append, false);
      if (append) {
        headers.addCopy(LowerCaseString(sh.header().key()), sh.header().value());
      } else {
        headers.setCopy(LowerCaseString(sh.header().key()), sh.header().value());
      }
    }
  }
}

void MutationUtils::applyCommonBodyResponse(const BodyResponse& response,
                                            Buffer::Instance& buffer) {
  if (response.has_response()) {
    const auto& common_response = response.response();
    if (common_response.has_body_mutation()) {
      applyBodyMutations(common_response.body_mutation(), buffer);
    }
  }
}

void MutationUtils::applyBodyMutations(const BodyMutation& mutation, Buffer::Instance& buffer) {
  switch (mutation.mutation_case()) {
  case BodyMutation::MutationCase::kClearBody:
    if (mutation.clear_body()) {
      buffer.drain(buffer.length());
    }
    break;
  case BodyMutation::MutationCase::kBody:
    buffer.drain(buffer.length());
    buffer.add(mutation.body());
    break;
  default:
    // Nothing to do on default
    break;
  }
}

// Ignore attempts to set certain sensitive headers that can break later processing.
// We may re-enable some of these after further testing. This logic is specific
// to the ext_proc filter so it is not shared with HeaderUtils.
bool MutationUtils::isSettableHeader(absl::string_view key) {
  const auto& headers = Headers::get();
  return !absl::EqualsIgnoreCase(key, headers.HostLegacy.get()) &&
         !absl::EqualsIgnoreCase(key, headers.Host.get()) &&
         !absl::EqualsIgnoreCase(key, headers.Method.get()) &&
         !absl::EqualsIgnoreCase(key, headers.Scheme.get()) &&
         !absl::StartsWithIgnoreCase(key, headers.prefix());
}

} // namespace ExternalProcessing
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy