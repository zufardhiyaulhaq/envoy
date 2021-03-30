#pragma once

#include <chrono>
#include <memory>
#include <string>

#include "envoy/event/timer.h"
#include "envoy/extensions/filters/http/ext_proc/v3alpha/ext_proc.pb.h"
#include "envoy/grpc/async_client.h"
#include "envoy/http/filter.h"
#include "envoy/service/ext_proc/v3alpha/external_processor.pb.h"
#include "envoy/stats/scope.h"
#include "envoy/stats/stats_macros.h"

#include "common/common/logger.h"

#include "extensions/filters/http/common/pass_through_filter.h"
#include "extensions/filters/http/ext_proc/client.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {
namespace ExternalProcessing {

#define ALL_EXT_PROC_FILTER_STATS(COUNTER)                                                         \
  COUNTER(streams_started)                                                                         \
  COUNTER(stream_msgs_sent)                                                                        \
  COUNTER(stream_msgs_received)                                                                    \
  COUNTER(spurious_msgs_received)                                                                  \
  COUNTER(streams_closed)                                                                          \
  COUNTER(streams_failed)                                                                          \
  COUNTER(failure_mode_allowed)                                                                    \
  COUNTER(message_timeouts)

struct ExtProcFilterStats {
  ALL_EXT_PROC_FILTER_STATS(GENERATE_COUNTER_STRUCT)
};

class FilterConfig {
public:
  FilterConfig(const envoy::extensions::filters::http::ext_proc::v3alpha::ExternalProcessor& config,
               const std::chrono::milliseconds message_timeout, Stats::Scope& scope,
               const std::string& stats_prefix)
      : failure_mode_allow_(config.failure_mode_allow()), message_timeout_(message_timeout),
        stats_(generateStats(stats_prefix, config.stat_prefix(), scope)),
        processing_mode_(config.processing_mode()) {}

  bool failureModeAllow() const { return failure_mode_allow_; }

  const std::chrono::milliseconds& messageTimeout() const { return message_timeout_; }

  const ExtProcFilterStats& stats() const { return stats_; }

  const envoy::extensions::filters::http::ext_proc::v3alpha::ProcessingMode&
  processingMode() const {
    return processing_mode_;
  }

private:
  ExtProcFilterStats generateStats(const std::string& prefix,
                                   const std::string& filter_stats_prefix, Stats::Scope& scope) {
    const std::string final_prefix = absl::StrCat(prefix, "ext_proc.", filter_stats_prefix);
    return {ALL_EXT_PROC_FILTER_STATS(POOL_COUNTER_PREFIX(scope, final_prefix))};
  }

  const bool failure_mode_allow_;
  const std::chrono::milliseconds message_timeout_;

  ExtProcFilterStats stats_;
  const envoy::extensions::filters::http::ext_proc::v3alpha::ProcessingMode processing_mode_;
};

using FilterConfigSharedPtr = std::shared_ptr<FilterConfig>;

class Filter : public Logger::Loggable<Logger::Id::filter>,
               public Http::PassThroughFilter,
               public ExternalProcessorCallbacks {
  // The state of filter execution -- this is used to determine
  // how to handle gRPC callbacks.
  enum class FilterState {
    // The filter is not waiting for anything, so any response on the
    // gRPC stream is spurious and will result in the filter closing
    // the stream.
    Idle,
    // The filter is waiting for a "request_headers" or a "response_headers" message.
    // Any other response on the gRPC stream will be treated as spurious.
    Headers,
    // The filter is waiting for a "request_body" or "response_body" message.
    // The body to modify is the filter's buffered body.
    BufferedBody,
  };

  // The result of an attempt to open the stream
  enum class StreamOpenState {
    // The stream was opened successfully
    Ok,
    // The stream was not opened successfully and an error was delivered
    // downstream -- processing should stop
    Error,
    // The stream was not opened successfully but processing should
    // continue as if the stream was already closed.
    IgnoreError,
  };

public:
  Filter(const FilterConfigSharedPtr& config, ExternalProcessorClientPtr&& client)
      : config_(config), client_(std::move(client)), stats_(config->stats()),
        processing_mode_(config->processingMode()) {}

  void onDestroy() override;

  Http::FilterHeadersStatus decodeHeaders(Http::RequestHeaderMap& headers,
                                          bool end_stream) override;
  Http::FilterDataStatus decodeData(Buffer::Instance& data, bool end_stream) override;

  Http::FilterHeadersStatus encodeHeaders(Http::ResponseHeaderMap& headers,
                                          bool end_stream) override;
  Http::FilterDataStatus encodeData(Buffer::Instance& data, bool end_stream) override;

  // ExternalProcessorCallbacks

  void onReceiveMessage(
      std::unique_ptr<envoy::service::ext_proc::v3alpha::ProcessingResponse>&& response) override;

  void onGrpcError(Grpc::Status::GrpcStatus error) override;

  void onGrpcClose() override;

private:
  StreamOpenState openStream();
  void startMessageTimer(Event::TimerPtr& timer);
  void onMessageTimeout();
  void cleanUpTimers();
  void clearAsyncState();
  void sendImmediateResponse(const envoy::service::ext_proc::v3alpha::ImmediateResponse& response);

  bool
  handleRequestHeadersResponse(const envoy::service::ext_proc::v3alpha::HeadersResponse& response);
  bool
  handleResponseHeadersResponse(const envoy::service::ext_proc::v3alpha::HeadersResponse& response);
  bool handleRequestBodyResponse(const envoy::service::ext_proc::v3alpha::BodyResponse& response);
  bool handleResponseBodyResponse(const envoy::service::ext_proc::v3alpha::BodyResponse& response);

  void sendBodyChunk(bool request_path, const Buffer::Instance& data, bool end_stream);

  const FilterConfigSharedPtr config_;
  const ExternalProcessorClientPtr client_;
  ExtProcFilterStats stats_;

  // The state of the request-processing, or "decoding" side of the filter.
  // We maintain separate states for encoding and decoding since they may
  // be interleaved.
  FilterState request_state_ = FilterState::Idle;

  // The state of the response-processing side
  FilterState response_state_ = FilterState::Idle;

  // The gRPC stream to the external processor, which will be opened
  // when it's time to send the first message.
  ExternalProcessorStreamPtr stream_;

  // Set to true when no more messages need to be sent to the processor.
  // This happens when the processor has closed the stream, or when it has
  // failed.
  bool processing_complete_ = false;

  // Set to true when an "immediate response" has been delivered. This helps us
  // know what response to return from certain failures.
  bool sent_immediate_response_ = false;

  // The headers that we'll be expected to modify. They are set when
  // received and reset to nullptr when they are no longer valid.
  Http::RequestHeaderMap* request_headers_ = nullptr;
  Http::ResponseHeaderMap* response_headers_ = nullptr;

  // The processing mode. May be locally overridden by any response,
  // So every instance of the filter has a copy.
  envoy::extensions::filters::http::ext_proc::v3alpha::ProcessingMode processing_mode_;

  // The timers that are used to manage per-message timeouts. Since the
  // request and response paths run in parallel and can theoretically overlap,
  // we need two timers.
  Event::TimerPtr request_message_timer_;
  Event::TimerPtr response_message_timer_;
};

} // namespace ExternalProcessing
} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy