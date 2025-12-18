// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <chrono>

// clang-format off
#include "opentelemetry/exporters/otlp/protobuf_include_prefix.h"
// clang-format on

#include "opentelemetry/proto/collector/trace/v1/trace_service.grpc.pb.h"

// clang-format off
#include "opentelemetry/exporters/otlp/protobuf_include_suffix.h"
// clang-format on

#include "opentelemetry/nostd/shared_ptr.h"
#include "opentelemetry/sdk/trace/exporter.h"

#include "opentelemetry/exporters/otlp/otlp_environment.h"
#include "opentelemetry/exporters/otlp/otlp_grpc_exporter_options.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace otlp
{

class OtlpGrpcClientReferenceGuard;

class OtlpGrpcClient;

/**
 * The OTLP exporter exports span data in OpenTelemetry Protocol (OTLP) format.
 */
class OtlpGrpcExporter final : public opentelemetry::sdk::trace::SpanExporter
{
public:
  /**
   * Create an OtlpGrpcExporter using all default options.
   */
  OtlpGrpcExporter();

  /**
   * Create an OtlpGrpcExporter using specified OtlpGrpcClient.
   *
   * @param options options to create exporter
   * @param client the gRPC client to use
   */
  OtlpGrpcExporter(const OtlpGrpcExporterOptions &options,
                   const std::shared_ptr<OtlpGrpcClient> &client);

  /**
   * Create an OtlpGrpcExporter using the given options.
   */
  explicit OtlpGrpcExporter(const OtlpGrpcExporterOptions &options);

  ~OtlpGrpcExporter() override;
  OtlpGrpcExporter(const OtlpGrpcExporter &)            = delete;
  OtlpGrpcExporter(OtlpGrpcExporter &&)                 = delete;
  OtlpGrpcExporter &operator=(const OtlpGrpcExporter &) = delete;
  OtlpGrpcExporter &operator=(OtlpGrpcExporter &&)      = delete;

  /**
   * Create a span recordable.
   * @return a newly initialized Recordable object
   */
  std::unique_ptr<sdk::trace::Recordable> MakeRecordable() noexcept override;

  /**
   * Export a batch of span recordables in OTLP format.
   * @param spans a span of unique pointers to span recordables
   */
  sdk::common::ExportResult Export(
      const nostd::span<std::unique_ptr<sdk::trace::Recordable>> &spans) noexcept override;

  /**
   * Force flush the exporter.
   * @param timeout an option timeout, default to max.
   * @return return true when all data are exported, and false when timeout
   */
  bool ForceFlush(
      std::chrono::microseconds timeout = (std::chrono::microseconds::max)()) noexcept override;

  /**
   * Shut down the exporter.
   * @param timeout an optional timeout, the default timeout of 0 means that no
   * timeout is applied.
   * @return return the status of this operation
   */
  bool Shutdown(
      std::chrono::microseconds timeout = (std::chrono::microseconds::max)()) noexcept override;

  /**
   * Get the Client object
   *
   * @return return binded gRPC client
   */
  const std::shared_ptr<OtlpGrpcClient> &GetClient() const noexcept;

private:
  // The configuration options associated with this exporter.
  const OtlpGrpcExporterOptions options_;

  std::shared_ptr<OtlpGrpcClient> client_;
  std::shared_ptr<OtlpGrpcClientReferenceGuard> client_reference_guard_;

  // For testing
  friend class OtlpGrpcExporterTestPeer;
  friend class OtlpGrpcLogRecordExporterTestPeer;

  // Store service stub internally. Useful for testing.
  std::shared_ptr<proto::collector::trace::v1::TraceService::StubInterface> trace_service_stub_;

  /**
   * Create an OtlpGrpcExporter using the specified service stub.
   * Only tests can call this constructor directly.
   * @param stub the service stub to be used for exporting
   */
  OtlpGrpcExporter(std::unique_ptr<proto::collector::trace::v1::TraceService::StubInterface> stub);

  /**
   * Create an OtlpGrpcExporter using the specified service stub and gRPC client.
   * Only tests can call this constructor directly.
   * @param stub the service stub to be used for exporting
   * @param client the gRPC client to use
   */
  OtlpGrpcExporter(std::unique_ptr<proto::collector::trace::v1::TraceService::StubInterface> stub,
                   const std::shared_ptr<OtlpGrpcClient> &client);

  std::atomic<bool> is_shutdown_{false};
  bool isShutdown() const noexcept;
};
}  // namespace otlp
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE
