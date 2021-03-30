#include "common/filter/http/filter_config_discovery_impl.h"

#include "envoy/config/core/v3/extension.pb.validate.h"
#include "envoy/server/filter_config.h"

#include "common/common/thread.h"
#include "common/config/utility.h"
#include "common/grpc/common.h"
#include "common/protobuf/utility.h"

#include "absl/strings/str_join.h"

namespace Envoy {
namespace Filter {
namespace Http {

DynamicFilterConfigProviderImpl::DynamicFilterConfigProviderImpl(
    FilterConfigSubscriptionSharedPtr& subscription,
    const absl::flat_hash_set<std::string>& require_type_urls,
    Server::Configuration::FactoryContext& factory_context)
    : subscription_(subscription), require_type_urls_(require_type_urls),
      tls_(factory_context.threadLocal()),
      init_target_("DynamicFilterConfigProviderImpl", [this]() {
        subscription_->start();
        // This init target is used to activate the subscription but not wait
        // for a response. It is used whenever a default config is provided to be
        // used while waiting for a response.
        init_target_.ready();
      }) {
  subscription_->filter_config_providers_.insert(this);
  tls_.set([](Event::Dispatcher&) { return std::make_shared<ThreadLocalConfig>(); });
}

DynamicFilterConfigProviderImpl::~DynamicFilterConfigProviderImpl() {
  subscription_->filter_config_providers_.erase(this);
}

void DynamicFilterConfigProviderImpl::validateTypeUrl(const std::string& type_url) const {
  if (!require_type_urls_.contains(type_url)) {
    throw EnvoyException(fmt::format("Error: filter config has type URL {} but expect {}.",
                                     type_url, absl::StrJoin(require_type_urls_, ", ")));
  }
}

const std::string& DynamicFilterConfigProviderImpl::name() { return subscription_->name(); }

absl::optional<Envoy::Http::FilterFactoryCb> DynamicFilterConfigProviderImpl::config() {
  return tls_->config_;
}

void DynamicFilterConfigProviderImpl::onConfigUpdate(Envoy::Http::FilterFactoryCb config,
                                                     const std::string&,
                                                     Config::ConfigAppliedCb cb) {
  tls_.runOnAllThreads(
      [config, cb](OptRef<ThreadLocalConfig> tls) {
        tls->config_ = config;
        if (cb) {
          cb();
        }
      },
      [this, config]() {
        // This happens after all workers have discarded the previous config so it can be safely
        // deleted on the main thread by an update with the new config.
        this->current_config_ = config;
      });
}

FilterConfigSubscription::FilterConfigSubscription(
    const envoy::config::core::v3::ConfigSource& config_source,
    const std::string& filter_config_name, Server::Configuration::FactoryContext& factory_context,
    const std::string& stat_prefix, FilterConfigProviderManagerImpl& filter_config_provider_manager,
    const std::string& subscription_id)
    : Config::SubscriptionBase<envoy::config::core::v3::TypedExtensionConfig>(
          envoy::config::core::v3::ApiVersion::V3,
          factory_context.messageValidationContext().dynamicValidationVisitor(), "name"),
      filter_config_name_(filter_config_name), factory_context_(factory_context),
      validator_(factory_context.messageValidationContext().dynamicValidationVisitor()),
      init_target_(fmt::format("FilterConfigSubscription init {}", filter_config_name_),
                   [this]() { start(); }),
      scope_(factory_context.scope().createScope(stat_prefix + "extension_config_discovery." +
                                                 filter_config_name_ + ".")),
      stat_prefix_(stat_prefix),
      stats_({ALL_EXTENSION_CONFIG_DISCOVERY_STATS(POOL_COUNTER(*scope_))}),
      filter_config_provider_manager_(filter_config_provider_manager),
      subscription_id_(subscription_id) {
  const auto resource_name = getResourceName();
  subscription_ =
      factory_context.clusterManager().subscriptionFactory().subscriptionFromConfigSource(
          config_source, Grpc::Common::typeUrl(resource_name), *scope_, *this, resource_decoder_,
          {});
}

void FilterConfigSubscription::start() {
  if (!started_) {
    started_ = true;
    subscription_->start({filter_config_name_});
  }
}

void FilterConfigSubscription::onConfigUpdate(
    const std::vector<Config::DecodedResourceRef>& resources, const std::string& version_info) {
  // Make sure to make progress in case the control plane is temporarily inconsistent.
  init_target_.ready();

  if (resources.size() != 1) {
    throw EnvoyException(fmt::format(
        "Unexpected number of resources in ExtensionConfigDS response: {}", resources.size()));
  }
  const auto& filter_config = dynamic_cast<const envoy::config::core::v3::TypedExtensionConfig&>(
      resources[0].get().resource());
  if (filter_config.name() != filter_config_name_) {
    throw EnvoyException(fmt::format("Unexpected resource name in ExtensionConfigDS response: {}",
                                     filter_config.name()));
  }
  // Skip update if hash matches
  const uint64_t new_hash = MessageUtil::hash(filter_config.typed_config());
  if (new_hash == last_config_hash_) {
    return;
  }
  auto& factory =
      Config::Utility::getAndCheckFactory<Server::Configuration::NamedHttpFilterConfigFactory>(
          filter_config);
  // Ensure that the filter config is valid in the filter chain context once the proto is processed.
  // Validation happens before updating to prevent a partial update application. It might be
  // possible that the providers have distinct type URL constraints.
  const auto type_url = Config::Utility::getFactoryType(filter_config.typed_config());
  for (auto* provider : filter_config_providers_) {
    provider->validateTypeUrl(type_url);
  }
  ProtobufTypes::MessagePtr message = Config::Utility::translateAnyToFactoryConfig(
      filter_config.typed_config(), validator_, factory);
  Envoy::Http::FilterFactoryCb factory_callback =
      factory.createFilterFactoryFromProto(*message, stat_prefix_, factory_context_);
  ENVOY_LOG(debug, "Updating filter config {}", filter_config_name_);
  const auto pending_update = std::make_shared<std::atomic<uint64_t>>(
      (factory_context_.admin().concurrency() + 1) * filter_config_providers_.size());
  for (auto* provider : filter_config_providers_) {
    provider->onConfigUpdate(factory_callback, version_info, [this, pending_update]() {
      if (--(*pending_update) == 0) {
        stats_.config_reload_.inc();
      }
    });
  }
  last_config_hash_ = new_hash;
  last_config_ = factory_callback;
  last_type_url_ = type_url;
  last_version_info_ = version_info;
}

void FilterConfigSubscription::onConfigUpdate(
    const std::vector<Config::DecodedResourceRef>& added_resources,
    const Protobuf::RepeatedPtrField<std::string>& removed_resources, const std::string&) {
  if (!removed_resources.empty()) {
    ENVOY_LOG(error,
              "Server sent a delta ExtensionConfigDS update attempting to remove a resource (name: "
              "{}). Ignoring.",
              removed_resources[0]);
  }
  if (!added_resources.empty()) {
    onConfigUpdate(added_resources, added_resources[0].get().version());
  }
}

void FilterConfigSubscription::onConfigUpdateFailed(Config::ConfigUpdateFailureReason reason,
                                                    const EnvoyException*) {
  ENVOY_LOG(debug, "Updating filter config {} failed due to {}", filter_config_name_, reason);
  stats_.config_fail_.inc();
  // Make sure to make progress in case the control plane is temporarily failing.
  init_target_.ready();
}

FilterConfigSubscription::~FilterConfigSubscription() {
  // If we get destroyed during initialization, make sure we signal that we "initialized".
  init_target_.ready();
  // Remove the subscription from the provider manager.
  filter_config_provider_manager_.subscriptions_.erase(subscription_id_);
}

void FilterConfigSubscription::incrementConflictCounter() { stats_.config_conflict_.inc(); }

std::shared_ptr<FilterConfigSubscription> FilterConfigProviderManagerImpl::getSubscription(
    const envoy::config::core::v3::ConfigSource& config_source, const std::string& name,
    Server::Configuration::FactoryContext& factory_context, const std::string& stat_prefix) {
  // FilterConfigSubscriptions are unique based on their config source and filter config name
  // combination.
  // TODO(https://github.com/envoyproxy/envoy/issues/11967) Hash collision can cause subscription
  // aliasing.
  const std::string subscription_id = absl::StrCat(MessageUtil::hash(config_source), ".", name);
  auto it = subscriptions_.find(subscription_id);
  if (it == subscriptions_.end()) {
    auto subscription = std::make_shared<FilterConfigSubscription>(
        config_source, name, factory_context, stat_prefix, *this, subscription_id);
    subscriptions_.insert({subscription_id, std::weak_ptr<FilterConfigSubscription>(subscription)});
    return subscription;
  } else {
    auto existing = it->second.lock();
    ASSERT(existing != nullptr,
           absl::StrCat("Cannot find subscribed filter config resource ", name));
    return existing;
  }
}

FilterConfigProviderPtr FilterConfigProviderManagerImpl::createDynamicFilterConfigProvider(
    const envoy::config::core::v3::ExtensionConfigSource& config_source,
    const std::string& filter_config_name, Server::Configuration::FactoryContext& factory_context,
    const std::string& stat_prefix) {
  auto subscription = getSubscription(config_source.config_source(), filter_config_name,
                                      factory_context, stat_prefix);
  // For warming, wait until the subscription receives the first response to indicate readiness.
  // Otherwise, mark ready immediately and start the subscription on initialization. A default
  // config is expected in the latter case.
  if (!config_source.apply_default_config_without_warming()) {
    factory_context.initManager().add(subscription->initTarget());
  }
  absl::flat_hash_set<std::string> require_type_urls;
  for (const auto& type_url : config_source.type_urls()) {
    auto factory_type_url = TypeUtil::typeUrlToDescriptorFullName(type_url);
    require_type_urls.emplace(factory_type_url);
  }
  auto provider = std::make_unique<DynamicFilterConfigProviderImpl>(subscription, require_type_urls,
                                                                    factory_context);
  // Ensure the subscription starts if it has not already.
  if (config_source.apply_default_config_without_warming()) {
    factory_context.initManager().add(provider->init_target_);
  }

  // If the subscription already received a config, attempt to apply it.
  // It is possible that the received extension config fails to satisfy the listener
  // type URL constraints. This may happen if ECDS and LDS updates are racing, and the LDS
  // update arrives first. In this case, use the default config, increment a metric,
  // and the applied config eventually converges once ECDS update arrives.
  bool last_config_valid = false;
  if (subscription->lastConfig().has_value()) {
    TRY_ASSERT_MAIN_THREAD {
      provider->validateTypeUrl(subscription->lastTypeUrl());
      last_config_valid = true;
    }
    END_TRY catch (const EnvoyException& e) {
      ENVOY_LOG(debug, "ECDS subscription {} is invalid in a listener context: {}.",
                filter_config_name, e.what());
      subscription->incrementConflictCounter();
    }
    if (last_config_valid) {
      provider->onConfigUpdate(subscription->lastConfig().value(), subscription->lastVersionInfo(),
                               nullptr);
    }
  }

  // Apply the default config if none has been applied.
  if (config_source.has_default_config() && !last_config_valid) {
    auto* default_factory =
        Config::Utility::getFactoryByType<Server::Configuration::NamedHttpFilterConfigFactory>(
            config_source.default_config());
    if (default_factory == nullptr) {
      throw EnvoyException(fmt::format("Error: cannot find filter factory {} for default filter "
                                       "configuration with type URL {}.",
                                       filter_config_name,
                                       config_source.default_config().type_url()));
    }
    provider->validateTypeUrl(Config::Utility::getFactoryType(config_source.default_config()));
    ProtobufTypes::MessagePtr message = Config::Utility::translateAnyToFactoryConfig(
        config_source.default_config(), factory_context.messageValidationVisitor(),
        *default_factory);
    Envoy::Http::FilterFactoryCb default_config =
        default_factory->createFilterFactoryFromProto(*message, stat_prefix, factory_context);
    provider->onConfigUpdate(default_config, "", nullptr);
  }
  return provider;
}

} // namespace Http
} // namespace Filter
} // namespace Envoy
