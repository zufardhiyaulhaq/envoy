#pragma once

#include <string>

#include "common/singleton/const_singleton.h"

namespace Envoy {
namespace Extensions {
namespace AccessLoggers {

/**
 * Well-known access logger names.
 * NOTE: New access loggers should use the well known name: envoy.access_loggers.name.
 */
class AccessLogNameValues {
public:
  // File access log
  const std::string File = "envoy.access_loggers.file";
  // HTTP gRPC access log
  const std::string HttpGrpc = "envoy.access_loggers.http_grpc";
  // Standard error access log
  const std::string Stderr = "envoy.access_loggers.stderr";
  // Standard output access log
  const std::string Stdout = "envoy.access_loggers.stdout";
  // TCP gRPC access log
  const std::string TcpGrpc = "envoy.access_loggers.tcp_grpc";
  // OpenTelemetry gRPC access log
  const std::string OpenTelemetry = "envoy.access_loggers.open_telemetry";
  // WASM access log
  const std::string Wasm = "envoy.access_loggers.wasm";
};

using AccessLogNames = ConstSingleton<AccessLogNameValues>;

} // namespace AccessLoggers
} // namespace Extensions
} // namespace Envoy
