#include <memory>

#include "envoy/config/endpoint/v3/endpoint.pb.h"
#include "envoy/config/endpoint/v3/endpoint.pb.validate.h"
#include "envoy/service/discovery/v3/discovery.pb.h"

#include "common/common/empty_string.h"
#include "common/config/new_grpc_mux_impl.h"
#include "common/config/protobuf_link_hacks.h"
#include "common/config/utility.h"
#include "common/config/version_converter.h"
#include "common/protobuf/protobuf.h"

#include "test/common/stats/stat_test_utility.h"
#include "test/mocks/common.h"
#include "test/mocks/config/mocks.h"
#include "test/mocks/event/mocks.h"
#include "test/mocks/grpc/mocks.h"
#include "test/mocks/local_info/mocks.h"
#include "test/mocks/runtime/mocks.h"
#include "test/test_common/logging.h"
#include "test/test_common/resources.h"
#include "test/test_common/simulated_time_system.h"
#include "test/test_common/test_runtime.h"
#include "test/test_common/test_time.h"
#include "test/test_common/utility.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

using testing::_;
using testing::InSequence;
using testing::Invoke;
using testing::NiceMock;
using testing::Return;
using testing::ReturnRef;

namespace Envoy {
namespace Config {
namespace {

// We test some mux specific stuff below, other unit test coverage for singleton use of
// NewGrpcMuxImpl is provided in [grpc_]subscription_impl_test.cc.
class NewGrpcMuxImplTestBase : public testing::Test {
public:
  NewGrpcMuxImplTestBase()
      : async_client_(new Grpc::MockAsyncClient()),
        control_plane_stats_(Utility::generateControlPlaneStats(stats_)),
        control_plane_connected_state_(
            stats_.gauge("control_plane.connected_state", Stats::Gauge::ImportMode::NeverImport)) {}

  void setup() {
    grpc_mux_ = std::make_unique<NewGrpcMuxImpl>(
        std::unique_ptr<Grpc::MockAsyncClient>(async_client_), dispatcher_,
        *Protobuf::DescriptorPool::generated_pool()->FindMethodByName(
            "envoy.service.discovery.v2.AggregatedDiscoveryService.StreamAggregatedResources"),
        envoy::config::core::v3::ApiVersion::AUTO, random_, stats_, rate_limit_settings_,
        local_info_);
  }

  void expectSendMessage(const std::string& type_url,
                         const std::vector<std::string>& resource_names_subscribe,
                         const std::vector<std::string>& resource_names_unsubscribe,
                         const std::string& nonce = "",
                         const Protobuf::int32 error_code = Grpc::Status::WellKnownGrpcStatus::Ok,
                         const std::string& error_message = "") {
    API_NO_BOOST(envoy::api::v2::DeltaDiscoveryRequest) expected_request;
    expected_request.mutable_node()->CopyFrom(API_DOWNGRADE(local_info_.node()));
    for (const auto& resource : resource_names_subscribe) {
      expected_request.add_resource_names_subscribe(resource);
    }
    for (const auto& resource : resource_names_unsubscribe) {
      expected_request.add_resource_names_unsubscribe(resource);
    }
    expected_request.set_response_nonce(nonce);
    expected_request.set_type_url(type_url);
    if (error_code != Grpc::Status::WellKnownGrpcStatus::Ok) {
      ::google::rpc::Status* error_detail = expected_request.mutable_error_detail();
      error_detail->set_code(error_code);
      error_detail->set_message(error_message);
    }
    EXPECT_CALL(async_stream_, sendMessageRaw_(Grpc::ProtoBufferEq(expected_request), false));
  }

  NiceMock<Event::MockDispatcher> dispatcher_;
  NiceMock<Random::MockRandomGenerator> random_;
  Grpc::MockAsyncClient* async_client_;
  NiceMock<Grpc::MockAsyncStream> async_stream_;
  NiceMock<LocalInfo::MockLocalInfo> local_info_;
  NewGrpcMuxImplPtr grpc_mux_;
  NiceMock<Config::MockSubscriptionCallbacks> callbacks_;
  TestUtility::TestOpaqueResourceDecoderImpl<envoy::config::endpoint::v3::ClusterLoadAssignment>
      resource_decoder_{"cluster_name"};
  Stats::TestUtil::TestStore stats_;
  Envoy::Config::RateLimitSettings rate_limit_settings_;
  ControlPlaneStats control_plane_stats_;
  Stats::Gauge& control_plane_connected_state_;
};

class NewGrpcMuxImplTest : public NewGrpcMuxImplTestBase {
public:
  Event::SimulatedTimeSystem time_system_;
};

// Validate behavior when dynamic context parameters are updated.
TEST_F(NewGrpcMuxImplTest, DynamicContextParameters) {
  setup();
  InSequence s;
  auto foo_sub = grpc_mux_->addWatch("foo", {"x", "y"}, callbacks_, resource_decoder_, {});
  auto bar_sub = grpc_mux_->addWatch("bar", {}, callbacks_, resource_decoder_, {});
  EXPECT_CALL(*async_client_, startRaw(_, _, _, _)).WillOnce(Return(&async_stream_));
  expectSendMessage("foo", {"x", "y"}, {});
  expectSendMessage("bar", {}, {});
  grpc_mux_->start();
  // Unknown type, shouldn't do anything.
  local_info_.context_provider_.update_cb_handler_.runCallbacks("baz");
  // Update to foo type should resend Node.
  expectSendMessage("foo", {}, {});
  local_info_.context_provider_.update_cb_handler_.runCallbacks("foo");
  // Update to bar type should resend Node.
  expectSendMessage("bar", {}, {});
  local_info_.context_provider_.update_cb_handler_.runCallbacks("bar");
  expectSendMessage("foo", {}, {"x", "y"});
}

// Test that we simply ignore a message for an unknown type_url, with no ill effects.
TEST_F(NewGrpcMuxImplTest, DiscoveryResponseNonexistentSub) {
  setup();

  const std::string& type_url = Config::TypeUrl::get().ClusterLoadAssignment;
  auto watch = grpc_mux_->addWatch(type_url, {}, callbacks_, resource_decoder_, {});

  EXPECT_CALL(*async_client_, startRaw(_, _, _, _)).WillOnce(Return(&async_stream_));
  grpc_mux_->start();

  {
    auto unexpected_response =
        std::make_unique<envoy::service::discovery::v3::DeltaDiscoveryResponse>();
    unexpected_response->set_type_url(type_url);
    unexpected_response->set_system_version_info("0");
    // empty response should call onConfigUpdate on wildcard watch
    EXPECT_CALL(callbacks_, onConfigUpdate(_, _, "0"));
    grpc_mux_->onDiscoveryResponse(std::move(unexpected_response), control_plane_stats_);
  }
  {
    auto response = std::make_unique<envoy::service::discovery::v3::DeltaDiscoveryResponse>();
    response->set_type_url(type_url);
    response->set_system_version_info("1");
    envoy::config::endpoint::v3::ClusterLoadAssignment load_assignment;
    load_assignment.set_cluster_name("x");
    response->add_resources()->mutable_resource()->PackFrom(load_assignment);
    EXPECT_CALL(callbacks_, onConfigUpdate(_, _, "1"))
        .WillOnce(Invoke([&load_assignment](const std::vector<DecodedResourceRef>& added_resources,
                                            const Protobuf::RepeatedPtrField<std::string>&,
                                            const std::string&) {
          EXPECT_EQ(1, added_resources.size());
          EXPECT_TRUE(
              TestUtility::protoEqual(added_resources[0].get().resource(), load_assignment));
        }));
    grpc_mux_->onDiscoveryResponse(std::move(response), control_plane_stats_);
  }
}

// DeltaDiscoveryResponse that comes in response to an on-demand request updates the watch with
// resource's name. The watch is initially created with an alias used in the on-demand request.
TEST_F(NewGrpcMuxImplTest, ConfigUpdateWithAliases) {
  setup();

  const std::string& type_url = Config::TypeUrl::get().VirtualHost;
  SubscriptionOptions options;
  options.use_namespace_matching_ = true;
  auto watch = grpc_mux_->addWatch(type_url, {"prefix"}, callbacks_, resource_decoder_, options);

  EXPECT_CALL(*async_client_, startRaw(_, _, _, _)).WillOnce(Return(&async_stream_));
  grpc_mux_->start();

  auto response = std::make_unique<envoy::service::discovery::v3::DeltaDiscoveryResponse>();
  response->set_type_url(type_url);
  response->set_system_version_info("1");

  envoy::config::route::v3::VirtualHost vhost;
  vhost.set_name("vhost_1");
  vhost.add_domains("domain1.test");
  vhost.add_domains("domain2.test");

  response->add_resources()->mutable_resource()->PackFrom(vhost);
  response->mutable_resources()->at(0).set_name("prefix/vhost_1");
  response->mutable_resources()->at(0).add_aliases("prefix/domain1.test");
  response->mutable_resources()->at(0).add_aliases("prefix/domain2.test");

  grpc_mux_->onDiscoveryResponse(std::move(response), control_plane_stats_);

  const auto& subscriptions = grpc_mux_->subscriptions();
  auto sub = subscriptions.find(type_url);

  EXPECT_TRUE(sub != subscriptions.end());
  watch->update({});
}

// DeltaDiscoveryResponse that comes in response to an on-demand request that couldn't be resolved
// will contain an empty Resource. The Resource's aliases field will be populated with the alias
// originally used in the request.
TEST_F(NewGrpcMuxImplTest, ConfigUpdateWithNotFoundResponse) {
  setup();

  const std::string& type_url = Config::TypeUrl::get().VirtualHost;
  SubscriptionOptions options;
  options.use_namespace_matching_ = true;
  auto watch = grpc_mux_->addWatch(type_url, {"prefix"}, callbacks_, resource_decoder_, options);

  EXPECT_CALL(*async_client_, startRaw(_, _, _, _)).WillOnce(Return(&async_stream_));
  grpc_mux_->start();

  auto response = std::make_unique<envoy::service::discovery::v3::DeltaDiscoveryResponse>();
  response->set_type_url(type_url);
  response->set_system_version_info("1");

  response->add_resources();
  response->mutable_resources()->at(0).set_name("not-found");
  response->mutable_resources()->at(0).add_aliases("prefix/domain1.test");
}

// Watch v2 resource type_url, receive discovery response with v3 resource type_url.
TEST_F(NewGrpcMuxImplTest, V3ResourceResponseV2ResourceWatch) {
  TestScopedRuntime scoped_runtime;
  Runtime::LoaderSingleton::getExisting()->mergeValues(
      {{"envoy.reloadable_features.enable_type_url_downgrade_and_upgrade", "true"}});
  setup();

  // Watch for v2 resource type_url.
  const std::string& v2_type_url = Config::TypeUrl::get().ClusterLoadAssignment;
  const std::string& v3_type_url =
      Config::getTypeUrl<envoy::config::endpoint::v3::ClusterLoadAssignment>(
          envoy::config::core::v3::ApiVersion::V3);
  auto watch = grpc_mux_->addWatch(v2_type_url, {}, callbacks_, resource_decoder_, {});

  EXPECT_CALL(*async_client_, startRaw(_, _, _, _)).WillOnce(Return(&async_stream_));
  // Cluster is not watched, v3 resource is rejected.
  grpc_mux_->start();
  {
    auto unexpected_response =
        std::make_unique<envoy::service::discovery::v3::DeltaDiscoveryResponse>();
    envoy::config::cluster::v3::Cluster cluster;
    unexpected_response->set_type_url(Config::getTypeUrl<envoy::config::cluster::v3::Cluster>(
        envoy::config::core::v3::ApiVersion::V3));
    unexpected_response->set_system_version_info("0");
    unexpected_response->add_resources()->mutable_resource()->PackFrom(cluster);
    EXPECT_CALL(callbacks_, onConfigUpdate(_, _, "0")).Times(0);
    grpc_mux_->onDiscoveryResponse(std::move(unexpected_response), control_plane_stats_);
  }
  // Cluster is not watched, v2 resource is rejected.
  {
    auto unexpected_response =
        std::make_unique<envoy::service::discovery::v3::DeltaDiscoveryResponse>();
    envoy::config::cluster::v3::Cluster cluster;
    unexpected_response->set_type_url(Config::TypeUrl::get().Cluster);
    unexpected_response->set_system_version_info("0");
    unexpected_response->add_resources()->mutable_resource()->PackFrom(cluster);
    EXPECT_CALL(callbacks_, onConfigUpdate(_, _, "0")).Times(0);
    grpc_mux_->onDiscoveryResponse(std::move(unexpected_response), control_plane_stats_);
  }
  // ClusterLoadAssignment v2 is watched, v3 resource will be accepted.
  {
    auto response = std::make_unique<envoy::service::discovery::v3::DeltaDiscoveryResponse>();
    response->set_system_version_info("1");
    envoy::config::endpoint::v3::ClusterLoadAssignment load_assignment;
    load_assignment.set_cluster_name("x");
    response->add_resources()->mutable_resource()->PackFrom(load_assignment);
    // Send response that contains resource with v3 type url.
    response->set_type_url(v3_type_url);
    EXPECT_CALL(callbacks_, onConfigUpdate(_, _, "1"))
        .WillOnce(Invoke([&load_assignment](const std::vector<DecodedResourceRef>& added_resources,
                                            const Protobuf::RepeatedPtrField<std::string>&,
                                            const std::string&) {
          EXPECT_EQ(1, added_resources.size());
          EXPECT_TRUE(
              TestUtility::protoEqual(added_resources[0].get().resource(), load_assignment));
        }));
    grpc_mux_->onDiscoveryResponse(std::move(response), control_plane_stats_);
  }
}

// Watch v3 resource type_url, receive discovery response with v2 resource type_url.
TEST_F(NewGrpcMuxImplTest, V2ResourceResponseV3ResourceWatch) {
  TestScopedRuntime scoped_runtime;
  Runtime::LoaderSingleton::getExisting()->mergeValues(
      {{"envoy.reloadable_features.enable_type_url_downgrade_and_upgrade", "true"}});
  setup();

  // Watch for v3 resource type_url.
  const std::string& v3_type_url =
      Config::getTypeUrl<envoy::config::endpoint::v3::ClusterLoadAssignment>(
          envoy::config::core::v3::ApiVersion::V3);
  const std::string& v2_type_url = Config::TypeUrl::get().ClusterLoadAssignment;
  auto watch = grpc_mux_->addWatch(v3_type_url, {}, callbacks_, resource_decoder_, {});

  EXPECT_CALL(*async_client_, startRaw(_, _, _, _)).WillOnce(Return(&async_stream_));

  grpc_mux_->start();
  // ClusterLoadAssignment v3 is watched, v2 resource will be accepted.
  {
    auto response = std::make_unique<envoy::service::discovery::v3::DeltaDiscoveryResponse>();
    response->set_system_version_info("1");
    envoy::config::endpoint::v3::ClusterLoadAssignment load_assignment;
    load_assignment.set_cluster_name("x");
    response->add_resources()->mutable_resource()->PackFrom(load_assignment);
    // Send response that contains resource with v3 type url.
    response->set_type_url(v2_type_url);
    EXPECT_CALL(callbacks_, onConfigUpdate(_, _, "1"))
        .WillOnce(Invoke([&load_assignment](const std::vector<DecodedResourceRef>& added_resources,
                                            const Protobuf::RepeatedPtrField<std::string>&,
                                            const std::string&) {
          EXPECT_EQ(1, added_resources.size());
          EXPECT_TRUE(
              TestUtility::protoEqual(added_resources[0].get().resource(), load_assignment));
        }));
    grpc_mux_->onDiscoveryResponse(std::move(response), control_plane_stats_);
  }
}

// Validate basic gRPC mux subscriptions to xdstp:// glob collections.
TEST_F(NewGrpcMuxImplTest, XdsTpGlobCollection) {
  setup();

  const std::string& type_url = Config::TypeUrl::get().ClusterLoadAssignment;
  xds::core::v3::ContextParams context_params;
  (*context_params.mutable_params())["foo"] = "bar";
  EXPECT_CALL(local_info_.context_provider_, nodeContext()).WillOnce(ReturnRef(context_params));
  // We verify that the gRPC mux normalizes the context parameter order below.
  SubscriptionOptions options;
  options.add_xdstp_node_context_params_ = true;
  auto watch = grpc_mux_->addWatch(
      type_url,
      {"xdstp://foo/envoy.config.endpoint.v3.ClusterLoadAssignment/bar/*?thing=some&some=thing"},
      callbacks_, resource_decoder_, options);

  EXPECT_CALL(*async_client_, startRaw(_, _, _, _)).WillOnce(Return(&async_stream_));
  grpc_mux_->start();

  auto response = std::make_unique<envoy::service::discovery::v3::DeltaDiscoveryResponse>();
  response->set_type_url(type_url);
  response->set_system_version_info("1");

  envoy::config::endpoint::v3::ClusterLoadAssignment load_assignment;
  load_assignment.set_cluster_name("ignore");
  auto* resource = response->add_resources();
  resource->set_name("xdstp://foo/envoy.config.endpoint.v3.ClusterLoadAssignment/bar/"
                     "a?foo=bar&some=thing&thing=some");
  resource->mutable_resource()->PackFrom(load_assignment);
  EXPECT_CALL(callbacks_, onConfigUpdate(_, _, "1"))
      .WillOnce(Invoke([&load_assignment](const std::vector<DecodedResourceRef>& added_resources,
                                          const Protobuf::RepeatedPtrField<std::string>&,
                                          const std::string&) {
        EXPECT_EQ(1, added_resources.size());
        EXPECT_TRUE(TestUtility::protoEqual(added_resources[0].get().resource(), load_assignment));
      }));
  grpc_mux_->onDiscoveryResponse(std::move(response), control_plane_stats_);
}

// Validate basic gRPC mux subscriptions to xdstp:// singletons.
TEST_F(NewGrpcMuxImplTest, XdsTpSingleton) {
  setup();

  const std::string& type_url = Config::TypeUrl::get().ClusterLoadAssignment;
  EXPECT_CALL(local_info_.context_provider_, nodeContext()).Times(0);
  // We verify that the gRPC mux normalizes the context parameter order below. Node context
  // parameters are skipped.
  SubscriptionOptions options;
  auto watch = grpc_mux_->addWatch(type_url,
                                   {
                                       "xdstp://foo/envoy.config.endpoint.v3.ClusterLoadAssignment/"
                                       "bar/baz?thing=some&some=thing",
                                       "opaque_resource_name",
                                       "xdstp://foo/envoy.config.endpoint.v3.ClusterLoadAssignment/"
                                       "bar/blah?thing=some&some=thing",
                                   },
                                   callbacks_, resource_decoder_, options);

  EXPECT_CALL(*async_client_, startRaw(_, _, _, _)).WillOnce(Return(&async_stream_));
  grpc_mux_->start();

  auto response = std::make_unique<envoy::service::discovery::v3::DeltaDiscoveryResponse>();
  response->set_type_url(type_url);
  response->set_system_version_info("1");

  envoy::config::endpoint::v3::ClusterLoadAssignment load_assignment;
  load_assignment.set_cluster_name("ignore");
  {
    auto* resource = response->add_resources();
    resource->set_name(
        "xdstp://foo/envoy.config.endpoint.v3.ClusterLoadAssignment/bar/baz?some=thing&thing=some");
    resource->mutable_resource()->PackFrom(load_assignment);
  }
  {
    auto* resource = response->add_resources();
    resource->set_name("opaque_resource_name");
    resource->mutable_resource()->PackFrom(load_assignment);
  }
  {
    auto* resource = response->add_resources();
    resource->set_name("xdstp://foo/envoy.config.endpoint.v3.ClusterLoadAssignment/bar/"
                       "blah?some=thing&thing=some");
    resource->mutable_resource()->PackFrom(load_assignment);
  }
  EXPECT_CALL(callbacks_, onConfigUpdate(_, _, "1"))
      .WillOnce(Invoke([&load_assignment](const std::vector<DecodedResourceRef>& added_resources,
                                          const Protobuf::RepeatedPtrField<std::string>&,
                                          const std::string&) {
        EXPECT_EQ(3, added_resources.size());
        EXPECT_TRUE(TestUtility::protoEqual(added_resources[0].get().resource(), load_assignment));
        EXPECT_TRUE(TestUtility::protoEqual(added_resources[1].get().resource(), load_assignment));
        EXPECT_TRUE(TestUtility::protoEqual(added_resources[2].get().resource(), load_assignment));
      }));
  grpc_mux_->onDiscoveryResponse(std::move(response), control_plane_stats_);
}

} // namespace
} // namespace Config
} // namespace Envoy
