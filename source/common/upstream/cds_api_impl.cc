#include "common/upstream/cds_api_impl.h"

#include <string>

#include "envoy/api/v2/cluster.pb.h"
#include "envoy/config/core/v3/config_source.pb.h"
#include "envoy/service/discovery/v3/discovery.pb.h"
#include "envoy/stats/scope.h"

#include "common/common/assert.h"
#include "common/common/cleanup.h"
#include "common/common/utility.h"
#include "common/config/api_version.h"
#include "common/config/utility.h"
#include "common/protobuf/utility.h"

#include "absl/container/node_hash_set.h"
#include "absl/strings/str_join.h"

namespace Envoy {
namespace Upstream {

CdsApiPtr CdsApiImpl::create(const envoy::config::core::v3::ConfigSource& cds_config,
                             const xds::core::v3::ResourceLocator* cds_resources_locator,
                             ClusterManager& cm, Stats::Scope& scope,
                             ProtobufMessage::ValidationVisitor& validation_visitor) {
  return CdsApiPtr{
      new CdsApiImpl(cds_config, cds_resources_locator, cm, scope, validation_visitor)};
}

CdsApiImpl::CdsApiImpl(const envoy::config::core::v3::ConfigSource& cds_config,
                       const xds::core::v3::ResourceLocator* cds_resources_locator,
                       ClusterManager& cm, Stats::Scope& scope,
                       ProtobufMessage::ValidationVisitor& validation_visitor)
    : Envoy::Config::SubscriptionBase<envoy::config::cluster::v3::Cluster>(
          cds_config.resource_api_version(), validation_visitor, "name"),
      cm_(cm), scope_(scope.createScope("cluster_manager.cds.")) {
  const auto resource_name = getResourceName();
  if (cds_resources_locator == nullptr) {
    subscription_ = cm_.subscriptionFactory().subscriptionFromConfigSource(
        cds_config, Grpc::Common::typeUrl(resource_name), *scope_, *this, resource_decoder_, {});
  } else {
    subscription_ = cm.subscriptionFactory().collectionSubscriptionFromUrl(
        *cds_resources_locator, cds_config, resource_name, *scope_, *this, resource_decoder_);
  }
}

void CdsApiImpl::onConfigUpdate(const std::vector<Config::DecodedResourceRef>& resources,
                                const std::string& version_info) {
  auto all_existing_clusters = cm_.clusters();
  // Exclude the clusters which CDS wants to add.
  for (const auto& resource : resources) {
    all_existing_clusters.active_clusters_.erase(resource.get().name());
    all_existing_clusters.warming_clusters_.erase(resource.get().name());
  }
  Protobuf::RepeatedPtrField<std::string> to_remove_repeated;
  for (const auto& [cluster_name, _] : all_existing_clusters.active_clusters_) {
    UNREFERENCED_PARAMETER(_);
    *to_remove_repeated.Add() = cluster_name;
  }
  for (const auto& [cluster_name, _] : all_existing_clusters.warming_clusters_) {
    UNREFERENCED_PARAMETER(_);
    // Do not add the cluster twice when the cluster is both active and warming.
    if (all_existing_clusters.active_clusters_.count(cluster_name) == 0) {
      *to_remove_repeated.Add() = cluster_name;
    }
  }
  onConfigUpdate(resources, to_remove_repeated, version_info);
}

void CdsApiImpl::onConfigUpdate(const std::vector<Config::DecodedResourceRef>& added_resources,
                                const Protobuf::RepeatedPtrField<std::string>& removed_resources,
                                const std::string& system_version_info) {
  Config::ScopedResume maybe_resume_eds;
  if (cm_.adsMux()) {
    const auto type_urls =
        Config::getAllVersionTypeUrls<envoy::config::endpoint::v3::ClusterLoadAssignment>();
    maybe_resume_eds = cm_.adsMux()->pause(type_urls);
  }

  ENVOY_LOG(info, "cds: add {} cluster(s), remove {} cluster(s)", added_resources.size(),
            removed_resources.size());

  std::vector<std::string> exception_msgs;
  absl::flat_hash_set<std::string> cluster_names(added_resources.size());
  bool any_applied = false;
  uint32_t added_or_updated = 0;
  uint32_t skipped = 0;
  for (const auto& resource : added_resources) {
    envoy::config::cluster::v3::Cluster cluster;
    TRY_ASSERT_MAIN_THREAD {
      cluster = dynamic_cast<const envoy::config::cluster::v3::Cluster&>(resource.get().resource());
      if (!cluster_names.insert(cluster.name()).second) {
        // NOTE: at this point, the first of these duplicates has already been successfully applied.
        throw EnvoyException(fmt::format("duplicate cluster {} found", cluster.name()));
      }
      if (cm_.addOrUpdateCluster(cluster, resource.get().version())) {
        any_applied = true;
        ENVOY_LOG(debug, "cds: add/update cluster '{}'", cluster.name());
        ++added_or_updated;
      } else {
        ENVOY_LOG(debug, "cds: add/update cluster '{}' skipped", cluster.name());
        ++skipped;
      }
    }
    END_TRY
    catch (const EnvoyException& e) {
      exception_msgs.push_back(fmt::format("{}: {}", cluster.name(), e.what()));
    }
  }
  for (const auto& resource_name : removed_resources) {
    if (cm_.removeCluster(resource_name)) {
      any_applied = true;
      ENVOY_LOG(debug, "cds: remove cluster '{}'", resource_name);
    }
  }

  ENVOY_LOG(info, "cds: added/updated {} cluster(s), skipped {} unmodified cluster(s)",
            added_or_updated, skipped);

  if (any_applied) {
    system_version_info_ = system_version_info;
  }
  runInitializeCallbackIfAny();
  if (!exception_msgs.empty()) {
    throw EnvoyException(
        fmt::format("Error adding/updating cluster(s) {}", absl::StrJoin(exception_msgs, ", ")));
  }
}

void CdsApiImpl::onConfigUpdateFailed(Envoy::Config::ConfigUpdateFailureReason reason,
                                      const EnvoyException*) {
  ASSERT(Envoy::Config::ConfigUpdateFailureReason::ConnectionFailure != reason);
  // We need to allow server startup to continue, even if we have a bad
  // config.
  runInitializeCallbackIfAny();
}

void CdsApiImpl::runInitializeCallbackIfAny() {
  if (initialize_callback_) {
    initialize_callback_();
    initialize_callback_ = nullptr;
  }
}

} // namespace Upstream
} // namespace Envoy
