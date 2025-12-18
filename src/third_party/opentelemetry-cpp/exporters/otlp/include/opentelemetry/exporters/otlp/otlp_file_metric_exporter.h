// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <memory>

#include "opentelemetry/exporters/otlp/otlp_file_client.h"
#include "opentelemetry/exporters/otlp/otlp_file_metric_exporter_options.h"
#include "opentelemetry/exporters/otlp/otlp_file_metric_exporter_runtime_options.h"
#include "opentelemetry/sdk/common/exporter_utils.h"
#include "opentelemetry/sdk/metrics/export/metric_producer.h"
#include "opentelemetry/sdk/metrics/instruments.h"
#include "opentelemetry/sdk/metrics/push_metric_exporter.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace otlp
{
/**
 * The OTLP exporter exports metrics data in OpenTelemetry Protocol (OTLP) format to file.
 */
class OtlpFileMetricExporter final : public opentelemetry::sdk::metrics::PushMetricExporter
{
public:
  /**
   * Create an OtlpFileMetricExporter with default exporter options.
   */
  OtlpFileMetricExporter();

  /**
   * Create an OtlpFileMetricExporter with user specified options.
   * @param options An object containing the user's configuration options.
   */
  OtlpFileMetricExporter(const OtlpFileMetricExporterOptions &options);

  /**
   * Create an OtlpFileMetricExporter with user specified options.
   * @param options An object containing the user's configuration options.
   * @param runtime_options An object containing the user's runtime options.
   */
  OtlpFileMetricExporter(const OtlpFileMetricExporterOptions &options,
                         const OtlpFileMetricExporterRuntimeOptions &runtime_options);

  /**
   * Get the AggregationTemporality for exporter
   *
   * @return AggregationTemporality
   */
  sdk::metrics::AggregationTemporality GetAggregationTemporality(
      sdk::metrics::InstrumentType instrument_type) const noexcept override;

  opentelemetry::sdk::common::ExportResult Export(
      const opentelemetry::sdk::metrics::ResourceMetrics &data) noexcept override;

  /**
   * Force flush the exporter.
   */
  bool ForceFlush(
      std::chrono::microseconds timeout = (std::chrono::microseconds::max)()) noexcept override;

  bool Shutdown(
      std::chrono::microseconds timeout = (std::chrono::microseconds::max)()) noexcept override;

private:
  friend class OtlpFileMetricExporterTestPeer;

  // Configuration options for the exporter
  OtlpFileMetricExporterOptions options_;
  // Runtime options for the exporter
  OtlpFileMetricExporterRuntimeOptions runtime_options_;

  // Aggregation Temporality Selector
  sdk::metrics::AggregationTemporalitySelector aggregation_temporality_selector_;

  // Object that stores the file context.
  std::unique_ptr<OtlpFileClient> file_client_;
};
}  // namespace otlp
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE
