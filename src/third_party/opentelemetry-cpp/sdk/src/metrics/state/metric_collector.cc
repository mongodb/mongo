// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <memory>
#include <ostream>
#include <utility>
#include <vector>

#include "opentelemetry/common/timestamp.h"
#include "opentelemetry/nostd/function_ref.h"
#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/sdk/common/global_log_handler.h"
#include "opentelemetry/sdk/instrumentationscope/instrumentation_scope.h"
#include "opentelemetry/sdk/metrics/data/metric_data.h"
#include "opentelemetry/sdk/metrics/export/metric_filter.h"
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

MetricCollector::MetricCollector(opentelemetry::sdk::metrics::MeterContext *context,
                                 std::shared_ptr<MetricReader> metric_reader,
                                 std::unique_ptr<MetricFilter> metric_filter)
    : meter_context_{context},
      metric_reader_{std::move(metric_reader)},
      metric_filter_(std::move(metric_filter))
{
  metric_reader_->SetMetricProducer(this);
}

AggregationTemporality MetricCollector::GetAggregationTemporality(
    InstrumentType instrument_type) noexcept
{
  auto aggregation_temporality = metric_reader_->GetAggregationTemporality(instrument_type);
  if (aggregation_temporality == AggregationTemporality::kDelta &&
      instrument_type == InstrumentType::kGauge)
  {
    OTEL_INTERNAL_LOG_ERROR(
        "[MetricCollector::GetAggregationTemporality] - Error getting aggregation temporality."
        << "Delta temporality for Synchronous Gauge is currently not supported, using cumulative "
           "temporality");

    return AggregationTemporality::kCumulative;
  }
  return aggregation_temporality;
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
    if (metric_data.empty())
    {
      return true;
    }
    ScopeMetrics scope_metrics;
    scope_metrics.metric_data_ = std::move(metric_data);
    scope_metrics.scope_       = meter->GetInstrumentationScope();
    if (!metric_filter_)
    {
      resource_metrics.scope_metric_data_.emplace_back(std::move(scope_metrics));
      return true;
    }

    ScopeMetrics filtered_scope_metrics;
    filtered_scope_metrics.scope_ = meter->GetInstrumentationScope();
    for (MetricData &metric : scope_metrics.metric_data_)
    {
      const opentelemetry::sdk::instrumentationscope::InstrumentationScope &scope =
          *scope_metrics.scope_;
      opentelemetry::nostd::string_view name = metric.instrument_descriptor.name_;
      const InstrumentType &type             = metric.instrument_descriptor.type_;
      opentelemetry::nostd::string_view unit = metric.instrument_descriptor.unit_;

      MetricFilter::MetricFilterResult metric_filter_result =
          metric_filter_->TestMetric(scope, name, type, unit);
      if (metric_filter_result == MetricFilter::MetricFilterResult::kAccept)
      {
        filtered_scope_metrics.metric_data_.emplace_back(std::move(metric));
        continue;
      }
      else if (metric_filter_result == MetricFilter::MetricFilterResult::kDrop)
      {
        continue;
      }

      std::vector<PointDataAttributes> filtered_point_data_attrs;
      for (PointDataAttributes &point_data_attr : metric.point_data_attr_)
      {
        const PointAttributes &attributes = point_data_attr.attributes;
        MetricFilter::AttributesFilterResult attributes_filter_result =
            metric_filter_->TestAttributes(scope, name, type, unit, attributes);
        if (attributes_filter_result == MetricFilter::AttributesFilterResult::kAccept)
        {
          filtered_point_data_attrs.emplace_back(std::move(point_data_attr));
        }
      }
      if (!filtered_point_data_attrs.empty())
      {
        metric.point_data_attr_ = std::move(filtered_point_data_attrs);
        filtered_scope_metrics.metric_data_.emplace_back(std::move(metric));
      }
    }
    if (!filtered_scope_metrics.metric_data_.empty())
    {
      resource_metrics.scope_metric_data_.emplace_back(std::move(filtered_scope_metrics));
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
