#pragma once

#include "envoy/common/pure.h"

#include "common/protobuf/protobuf.h"

#include "absl/types/optional.h"

namespace Envoy {
namespace Config {

using ConfigAppliedCb = std::function<void()>;

/**
 * A provider for extension configurations obtained either statically or via
 * the extension configuration discovery service. Dynamically updated extension
 * configurations may share subscriptions across extension config providers.
 */
template <class Factory, class FactoryCallback> class ExtensionConfigProvider {
public:
  virtual ~ExtensionConfigProvider() = default;

  /**
   * Get the extension configuration resource name.
   **/
  virtual const std::string& name() PURE;

  /**
   * @return FactoryCallback an extension factory callback. Note that if the
   * provider has not yet performed an initial configuration load and no
   * default is provided, an empty optional will be returned. The factory
   * callback is the latest version of the extension configuration, and should
   * generally apply only to new requests and connections.
   */
  virtual absl::optional<FactoryCallback> config() PURE;

  /**
   * Update the provider with a new configuration.
   * @param config is an extension factory callback to replace the existing configuration.
   * @param version_info is the version of the new extension configuration.
   * @param cb the continuation callback for a completed configuration application.
   */
  virtual void onConfigUpdate(FactoryCallback config, const std::string& version_info,
                              ConfigAppliedCb cb) PURE;
};

} // namespace Config
} // namespace Envoy
