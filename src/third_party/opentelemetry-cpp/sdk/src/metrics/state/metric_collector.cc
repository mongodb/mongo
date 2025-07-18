// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <chrono>
#include <memory>
#include <ostream>
#include <utility>
#include <vector>

#include "opentelemetry/nostd/function_ref.h"
#include "opentelemetry/sdk/common/global_log_handler.h"
#include "opentelemetry/sdk/metrics/data/metric_data.h"
#include "opentelemetry/sdk/metrics/export/metric_producer.h"
#include "opentelemetry/sdk/metrics/instruments.h"
#include "opentelemetry/sdk/metrics/meter.h"
#include "opentelemetry/sdk/metrics/meter_context.h"
#include "opentelemetry/sdk/metrics/metric_reader.h"
#include "opentelemetry/sdk/metrics/state/metric_collector.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{
using opentelemetry::sdk::resource::Resource;

MetricCollector::MetricCollector(opentelemetry::sdk::metrics::MeterContext *context,
                                 std::shared_ptr<MetricReader> metric_reader)
    : meter_context_{context}, metric_reader_{std::move(metric_reader)}
{
  metric_reader_->SetMetricProducer(this);
}

AggregationTemporality MetricCollector::GetAggregationTemporality(
    InstrumentType instrument_type) noexcept
{
  return metric_reader_->GetAggregationTemporality(instrument_type);
}

MetricProducer::Result MetricCollector::Produce() noexcept
{
  if (!meter_context_)
  {
    OTEL_INTERNAL_LOG_ERROR("[MetricCollector::Collect] - Error during collecting."
                            << "The metric context is invalid");
    return {{}, MetricProducer::Status::kFailure};
  }
  ResourceMetrics resource_metrics;
  meter_context_->ForEachMeter([&](const std::shared_ptr<Meter> &meter) noexcept {
    auto collection_ts = std::chrono::system_clock::now();
    auto metric_data   = meter->Collect(this, collection_ts);
    if (!metric_data.empty())
    {
      ScopeMetrics scope_metrics;
      scope_metrics.metric_data_ = std::move(metric_data);
      scope_metrics.scope_       = meter->GetInstrumentationScope();
      resource_metrics.scope_metric_data_.emplace_back(std::move(scope_metrics));
    }
    return true;
  });
  resource_metrics.resource_ = &meter_context_->GetResource();
  return {resource_metrics, MetricProducer::Status::kSuccess};
}

bool MetricCollector::ForceFlush(std::chrono::microseconds timeout) noexcept
{
  return metric_reader_->ForceFlush(timeout);
}

bool MetricCollector::Shutdown(std::chrono::microseconds timeout) noexcept
{
  return metric_reader_->Shutdown(timeout);
}

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
