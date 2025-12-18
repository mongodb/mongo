// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <chrono>
#include <memory>

#include "opentelemetry/exporters/memory/in_memory_metric_data.h"
#include "opentelemetry/exporters/memory/in_memory_metric_exporter_factory.h"
#include "opentelemetry/sdk/common/exporter_utils.h"
#include "opentelemetry/sdk/common/global_log_handler.h"
#include "opentelemetry/sdk/metrics/export/metric_producer.h"
#include "opentelemetry/sdk/metrics/instruments.h"
#include "opentelemetry/sdk/metrics/push_metric_exporter.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace memory
{

using opentelemetry::sdk::metrics::PushMetricExporter;
using sdk::common::ExportResult;
using sdk::metrics::AggregationTemporality;
using sdk::metrics::AggregationTemporalitySelector;
using sdk::metrics::InstrumentType;
using sdk::metrics::ResourceMetrics;

namespace
{

/// A Push Metric Exporter which accumulates metrics data in memory and allows it to be inspected.
/// It is not thread-safe.
class InMemoryMetricExporter final : public sdk::metrics::PushMetricExporter
{
public:
  /// @param data The in-memory data to export to.
  /// @param temporality Output temporality as a function of instrument kind.
  InMemoryMetricExporter(const std::shared_ptr<InMemoryMetricData> &data,
                         const sdk::metrics::AggregationTemporalitySelector &temporality)
      : data_(data), temporality_(temporality)
  {}

  ~InMemoryMetricExporter() override = default;

  InMemoryMetricExporter(const InMemoryMetricExporter &)  = delete;
  InMemoryMetricExporter(const InMemoryMetricExporter &&) = delete;
  void operator=(const InMemoryMetricExporter &)          = delete;
  void operator=(const InMemoryMetricExporter &&)         = delete;

  ExportResult Export(const ResourceMetrics &data) noexcept override
  {
    if (is_shutdown_)
    {
      OTEL_INTERNAL_LOG_ERROR("[In Memory Metric Exporter] Exporting failed, exporter is shutdown");
      return ExportResult::kFailure;
    }
    data_->Add(std::unique_ptr<ResourceMetrics>(new ResourceMetrics{data}));
    return ExportResult::kSuccess;
  }

  AggregationTemporality GetAggregationTemporality(
      InstrumentType instrument_type) const noexcept override
  {
    return temporality_(instrument_type);
  }

  bool ForceFlush(std::chrono::microseconds /* timeout */) noexcept override { return true; }

  bool Shutdown(std::chrono::microseconds /* timeout */) noexcept override
  {
    is_shutdown_ = true;
    return true;
  }

private:
  std::shared_ptr<InMemoryMetricData> data_;
  std::atomic<bool> is_shutdown_{false};
  sdk::metrics::AggregationTemporalitySelector temporality_;
};

}  // namespace

std::unique_ptr<PushMetricExporter> InMemoryMetricExporterFactory::Create(
    const std::shared_ptr<InMemoryMetricData> &data)
{
  return Create(data,
                [](sdk::metrics::InstrumentType) { return AggregationTemporality::kCumulative; });
}

std::unique_ptr<PushMetricExporter> InMemoryMetricExporterFactory::Create(
    const std::shared_ptr<InMemoryMetricData> &data,
    const AggregationTemporalitySelector &temporality)
{
  return std::unique_ptr<InMemoryMetricExporter>(new InMemoryMetricExporter{data, temporality});
}

}  // namespace memory
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE
