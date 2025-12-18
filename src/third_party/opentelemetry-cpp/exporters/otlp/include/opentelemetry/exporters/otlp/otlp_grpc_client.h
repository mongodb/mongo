// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <grpcpp/grpcpp.h>
#include <grpcpp/support/status.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>

#include "opentelemetry/exporters/otlp/otlp_grpc_client_options.h"
#include "opentelemetry/version.h"

// clang-format off
#include "opentelemetry/exporters/otlp/protobuf_include_prefix.h" // IWYU pragma: keep
#include "opentelemetry/proto/collector/logs/v1/logs_service.grpc.pb.h"
#include "opentelemetry/proto/collector/metrics/v1/metrics_service.grpc.pb.h"
#include "opentelemetry/proto/collector/trace/v1/trace_service.grpc.pb.h"
#include "opentelemetry/exporters/otlp/protobuf_include_suffix.h" // IWYU pragma: keep
// clang-format on

#ifdef ENABLE_ASYNC_EXPORT
#  include <functional>

#  include "opentelemetry/sdk/common/exporter_utils.h"
#endif /* ENABLE_ASYNC_EXPORT */

namespace google
{
namespace protobuf
{
class Arena;
}
}  // namespace google

namespace opentelemetry
{
namespace proto
{
namespace collector
{

namespace logs
{
namespace v1
{
class ExportLogsServiceRequest;
class ExportLogsServiceResponse;
}  // namespace v1
}  // namespace logs

namespace metrics
{
namespace v1
{
class ExportMetricsServiceRequest;
class ExportMetricsServiceResponse;
}  // namespace v1
}  // namespace metrics

namespace trace
{
namespace v1
{
class ExportTraceServiceRequest;
class ExportTraceServiceResponse;
}  // namespace v1

}  // namespace trace

}  // namespace collector
}  // namespace proto
}  // namespace opentelemetry

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace otlp
{

struct OtlpGrpcClientOptions;    // IWYU pragma: keep
struct OtlpGrpcClientAsyncData;  // IWYU pragma: keep

class OtlpGrpcClientReferenceGuard
{
public:
  OtlpGrpcClientReferenceGuard() noexcept;
  ~OtlpGrpcClientReferenceGuard() noexcept;

  OtlpGrpcClientReferenceGuard(const OtlpGrpcClientReferenceGuard &)            = delete;
  OtlpGrpcClientReferenceGuard(OtlpGrpcClientReferenceGuard &&)                 = delete;
  OtlpGrpcClientReferenceGuard &operator=(const OtlpGrpcClientReferenceGuard &) = delete;
  OtlpGrpcClientReferenceGuard &operator=(OtlpGrpcClientReferenceGuard &&)      = delete;

private:
  friend class OtlpGrpcClient;
  std::atomic<bool> has_value_;
};

/**
 * The OTLP gRPC client contains utility functions of gRPC.
 */
class OtlpGrpcClient
{
public:
  OtlpGrpcClient(const OtlpGrpcClientOptions &options);
  ~OtlpGrpcClient();
  OtlpGrpcClient(const OtlpGrpcClient &)            = delete;
  OtlpGrpcClient(OtlpGrpcClient &&)                 = delete;
  OtlpGrpcClient &operator=(const OtlpGrpcClient &) = delete;
  OtlpGrpcClient &operator=(OtlpGrpcClient &&)      = delete;

  static std::string GetGrpcTarget(const std::string &endpoint);

  /**
   * Create gRPC channel from the exporter options.
   */
  static std::shared_ptr<grpc::Channel> MakeChannel(const OtlpGrpcClientOptions &options);

  /**
   * Create gRPC client context to call RPC.
   */
  static std::unique_ptr<grpc::ClientContext> MakeClientContext(
      const OtlpGrpcClientOptions &options);

  /**
   * Create trace service stub to communicate with the OpenTelemetry Collector.
   */
  std::unique_ptr<proto::collector::trace::v1::TraceService::StubInterface> MakeTraceServiceStub();

  /**
   * Create metrics service stub to communicate with the OpenTelemetry Collector.
   */
  std::unique_ptr<proto::collector::metrics::v1::MetricsService::StubInterface>
  MakeMetricsServiceStub();

  /**
   * Create logs service stub to communicate with the OpenTelemetry Collector.
   */
  std::unique_ptr<proto::collector::logs::v1::LogsService::StubInterface> MakeLogsServiceStub();

  static grpc::Status DelegateExport(
      proto::collector::trace::v1::TraceService::StubInterface *stub,
      std::unique_ptr<grpc::ClientContext> &&context,
      std::unique_ptr<google::protobuf::Arena> &&arena,
      proto::collector::trace::v1::ExportTraceServiceRequest &&request,
      proto::collector::trace::v1::ExportTraceServiceResponse *response);

  static grpc::Status DelegateExport(
      proto::collector::metrics::v1::MetricsService::StubInterface *stub,
      std::unique_ptr<grpc::ClientContext> &&context,
      std::unique_ptr<google::protobuf::Arena> &&arena,
      proto::collector::metrics::v1::ExportMetricsServiceRequest &&request,
      proto::collector::metrics::v1::ExportMetricsServiceResponse *response);

  static grpc::Status DelegateExport(
      proto::collector::logs::v1::LogsService::StubInterface *stub,
      std::unique_ptr<grpc::ClientContext> &&context,
      std::unique_ptr<google::protobuf::Arena> &&arena,
      proto::collector::logs::v1::ExportLogsServiceRequest &&request,
      proto::collector::logs::v1::ExportLogsServiceResponse *response);

  void AddReference(OtlpGrpcClientReferenceGuard &guard,
                    const OtlpGrpcClientOptions &options) noexcept;

  /**
   * Reomve reference fro a guard object
   *
   * @param guard guard object to remove reference from
   * @return true if there is no more reference to this gRPC client
   */
  bool RemoveReference(OtlpGrpcClientReferenceGuard &guard) noexcept;

#ifdef ENABLE_ASYNC_EXPORT
  /**
   * Async export
   * @param options Options used to message to create gRPC context and stub(if necessary)
   * @param arena Protobuf arena to hold lifetime of all messages
   * @param request Request for this RPC
   * @param result_callback callback to call when the exporting is done
   * @return return the status of this operation
   */
  sdk::common::ExportResult DelegateAsyncExport(
      const OtlpGrpcClientOptions &options,
      proto::collector::trace::v1::TraceService::StubInterface *stub,
      std::unique_ptr<grpc::ClientContext> &&context,
      std::unique_ptr<google::protobuf::Arena> &&arena,
      proto::collector::trace::v1::ExportTraceServiceRequest &&request,
      std::function<bool(opentelemetry::sdk::common::ExportResult,
                         std::unique_ptr<google::protobuf::Arena> &&,
                         const proto::collector::trace::v1::ExportTraceServiceRequest &,
                         proto::collector::trace::v1::ExportTraceServiceResponse *)>
          &&result_callback) noexcept;

  /**
   * Async export
   * @param options Options used to message to create gRPC context and stub(if necessary)
   * @param arena Protobuf arena to hold lifetime of all messages
   * @param request Request for this RPC
   * @param result_callback callback to call when the exporting is done
   * @return return the status of this operation
   */
  sdk::common::ExportResult DelegateAsyncExport(
      const OtlpGrpcClientOptions &options,
      proto::collector::metrics::v1::MetricsService::StubInterface *stub,
      std::unique_ptr<grpc::ClientContext> &&context,
      std::unique_ptr<google::protobuf::Arena> &&arena,
      proto::collector::metrics::v1::ExportMetricsServiceRequest &&request,
      std::function<bool(opentelemetry::sdk::common::ExportResult,
                         std::unique_ptr<google::protobuf::Arena> &&,
                         const proto::collector::metrics::v1::ExportMetricsServiceRequest &,
                         proto::collector::metrics::v1::ExportMetricsServiceResponse *)>
          &&result_callback) noexcept;

  /**
   * Async export
   * @param options Options used to message to create gRPC context and stub(if necessary)
   * @param arena Protobuf arena to hold lifetime of all messages
   * @param request Request for this RPC
   * @param result_callback callback to call when the exporting is done
   * @return return the status of this operation
   */
  sdk::common::ExportResult DelegateAsyncExport(
      const OtlpGrpcClientOptions &options,
      proto::collector::logs::v1::LogsService::StubInterface *stub,
      std::unique_ptr<grpc::ClientContext> &&context,
      std::unique_ptr<google::protobuf::Arena> &&arena,
      proto::collector::logs::v1::ExportLogsServiceRequest &&request,
      std::function<bool(opentelemetry::sdk::common::ExportResult,
                         std::unique_ptr<google::protobuf::Arena> &&,
                         const proto::collector::logs::v1::ExportLogsServiceRequest &,
                         proto::collector::logs::v1::ExportLogsServiceResponse *)>
          &&result_callback) noexcept;
#endif

  /**
   * Force flush the gRPC client.
   */
  bool ForceFlush(std::chrono::microseconds timeout = (std::chrono::microseconds::max)()) noexcept;

  /**
   * Shut down the gRPC client.
   * @param timeout an optional timeout, the default timeout of 0 means that no
   * timeout is applied.
   * @return return the status of this operation
   */
  bool Shutdown(OtlpGrpcClientReferenceGuard &guard,
                std::chrono::microseconds timeout = std::chrono::microseconds(0)) noexcept;

  std::shared_ptr<OtlpGrpcClientAsyncData> MutableAsyncData(const OtlpGrpcClientOptions &options);

  bool IsShutdown() const noexcept;

private:
  // Stores if this gRPC client had its Shutdown() method called
  std::atomic<bool> is_shutdown_;

  // Stores shared data between threads of this gRPC client
  std::shared_ptr<OtlpGrpcClientAsyncData> async_data_;
};
}  // namespace otlp
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE
