// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

// clang-format off

#include "opentelemetry/exporters/otlp/protobuf_include_prefix.h"
#include "opentelemetry/proto/collector/metrics/v1/metrics_service.grpc.pb.h"
#include "opentelemetry/exporters/otlp/protobuf_include_suffix.h"

// clang-format on

#include "opentelemetry/exporters/otlp/otlp_environment.h"
#include "opentelemetry/exporters/otlp/otlp_grpc_metric_exporter_options.h"
#include "opentelemetry/sdk/metrics/push_metric_exporter.h"

#include <atomic>

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace otlp
{

class OtlpGrpcClient;

/**
 * The OTLP exporter exports metrics data in OpenTelemetry Protocol (OTLP) format in gRPC.
 */
class OtlpGrpcMetricExporter : public opentelemetry::sdk::metrics::PushMetricExporter
{
public:
  /**
   * Create an OtlpGrpcMetricExporter using all default options.
   */
  OtlpGrpcMetricExporter();

  /**
   * Create an OtlpGrpcMetricExporter using the given options.
   */
  explicit OtlpGrpcMetricExporter(const OtlpGrpcMetricExporterOptions &options);

  /**
   * Get the AggregationTemporality for exporter
   *
   * @return AggregationTemporality
   */
  sdk::metrics::AggregationTemporality GetAggregationTemporality(
      sdk::metrics::InstrumentType instrument_type) const noexcept override;

  opentelemetry::sdk::common::ExportResult Export(
      const opentelemetry::sdk::metrics::ResourceMetrics &data) noexcept override;

  bool ForceFlush(
      std::chrono::microseconds timeout = (std::chrono::microseconds::max)()) noexcept override;

  bool Shutdown(
      std::chrono::microseconds timeout = (std::chrono::microseconds::max)()) noexcept override;

private:
  // The configuration options associated with this exporter.
  const OtlpGrpcMetricExporterOptions options_;

#ifdef ENABLE_ASYNC_EXPORT
  std::shared_ptr<OtlpGrpcClient> client_;
#endif

  // Aggregation Temporality selector
  const sdk::metrics::AggregationTemporalitySelector aggregation_temporality_selector_;

  // For testing
  friend class OtlpGrpcMetricExporterTestPeer;

  // Store service stub internally. Useful for testing.
  std::unique_ptr<proto::collector::metrics::v1::MetricsService::StubInterface>
      metrics_service_stub_;

  /**
   * Create an OtlpGrpcMetricExporter using the specified service stub.
   * Only tests can call this constructor directly.
   * @param stub the service stub to be used for exporting
   */
  OtlpGrpcMetricExporter(
      std::unique_ptr<proto::collector::metrics::v1::MetricsService::StubInterface> stub);
  std::atomic<bool> is_shutdown_{false};
  bool isShutdown() const noexcept;
};
}  // namespace otlp
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE
