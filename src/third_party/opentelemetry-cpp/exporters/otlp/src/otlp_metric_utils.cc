// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <stdint.h>
#include <chrono>
#include <map>
#include <utility>
#include <vector>

#include "opentelemetry/common/timestamp.h"
#include "opentelemetry/exporters/otlp/otlp_metric_utils.h"
#include "opentelemetry/exporters/otlp/otlp_populate_attribute_utils.h"
#include "opentelemetry/exporters/otlp/otlp_preferred_temporality.h"
#include "opentelemetry/nostd/variant.h"
#include "third_party/opentelemetry-cpp/sdk/include/opentelemetry/sdk/common/attribute_utils.h"
#include "third_party/opentelemetry-cpp/sdk/include/opentelemetry/sdk/instrumentationscope/instrumentation_scope.h"
#include "third_party/opentelemetry-cpp/sdk/include/opentelemetry/sdk/metrics/data/metric_data.h"
#include "third_party/opentelemetry-cpp/sdk/include/opentelemetry/sdk/metrics/data/point_data.h"
#include "third_party/opentelemetry-cpp/sdk/include/opentelemetry/sdk/metrics/export/metric_producer.h"
#include "third_party/opentelemetry-cpp/sdk/include/opentelemetry/sdk/metrics/instruments.h"
#include "third_party/opentelemetry-cpp/api/include/opentelemetry/version.h"

// clang-format off
#include "opentelemetry/exporters/otlp/protobuf_include_prefix.h" // IWYU pragma: keep
#include "src/third_party/opentelemetry-proto/opentelemetry/proto/collector/metrics/v1/metrics_service.pb.h"
#include "src/third_party/opentelemetry-proto/opentelemetry/proto/common/v1/common.pb.h"
#include "src/third_party/opentelemetry-proto/opentelemetry/proto/metrics/v1/metrics.pb.h"
#include "opentelemetry/exporters/otlp/protobuf_include_suffix.h" // IWYU pragma: keep
// clang-format on

OPENTELEMETRY_BEGIN_NAMESPACE

namespace exporter
{
namespace otlp
{
namespace metric_sdk = opentelemetry::sdk::metrics;

proto::metrics::v1::AggregationTemporality OtlpMetricUtils::GetProtoAggregationTemporality(
    const opentelemetry::sdk::metrics::AggregationTemporality &aggregation_temporality) noexcept
{
  if (aggregation_temporality == opentelemetry::sdk::metrics::AggregationTemporality::kCumulative)
    return proto::metrics::v1::AggregationTemporality::AGGREGATION_TEMPORALITY_CUMULATIVE;
  else if (aggregation_temporality == opentelemetry::sdk::metrics::AggregationTemporality::kDelta)
    return proto::metrics::v1::AggregationTemporality::AGGREGATION_TEMPORALITY_DELTA;
  else
    return proto::metrics::v1::AggregationTemporality::AGGREGATION_TEMPORALITY_UNSPECIFIED;
}

metric_sdk::AggregationType OtlpMetricUtils::GetAggregationType(
    const opentelemetry::sdk::metrics::MetricData &metric_data) noexcept
{
  if (metric_data.point_data_attr_.size() == 0)
  {
    return metric_sdk::AggregationType::kDrop;
  }
  auto point_data_with_attributes = metric_data.point_data_attr_[0];
  if (nostd::holds_alternative<sdk::metrics::SumPointData>(point_data_with_attributes.point_data))
  {
    return metric_sdk::AggregationType::kSum;
  }
  else if (nostd::holds_alternative<sdk::metrics::HistogramPointData>(
               point_data_with_attributes.point_data))
  {
    return metric_sdk::AggregationType::kHistogram;
  }
  else if (nostd::holds_alternative<sdk::metrics::LastValuePointData>(
               point_data_with_attributes.point_data))
  {
    return metric_sdk::AggregationType::kLastValue;
  }
  return metric_sdk::AggregationType::kDrop;
}

void OtlpMetricUtils::ConvertSumMetric(const metric_sdk::MetricData &metric_data,
                                       proto::metrics::v1::Sum *const sum) noexcept
{
  sum->set_aggregation_temporality(
      GetProtoAggregationTemporality(metric_data.aggregation_temporality));
  sum->set_is_monotonic(
      (metric_data.instrument_descriptor.type_ == metric_sdk::InstrumentType::kCounter) ||
      (metric_data.instrument_descriptor.type_ == metric_sdk::InstrumentType::kObservableCounter));
  auto start_ts = metric_data.start_ts.time_since_epoch().count();
  auto ts       = metric_data.end_ts.time_since_epoch().count();
  for (auto &point_data_with_attributes : metric_data.point_data_attr_)
  {
    proto::metrics::v1::NumberDataPoint *proto_sum_point_data = sum->add_data_points();
    proto_sum_point_data->set_start_time_unix_nano(start_ts);
    proto_sum_point_data->set_time_unix_nano(ts);
    auto sum_data = nostd::get<sdk::metrics::SumPointData>(point_data_with_attributes.point_data);

    if ((nostd::holds_alternative<int64_t>(sum_data.value_)))
    {
      proto_sum_point_data->set_as_int(nostd::get<int64_t>(sum_data.value_));
    }
    else
    {
      proto_sum_point_data->set_as_double(nostd::get<double>(sum_data.value_));
    }
    // set attributes
    for (auto &kv_attr : point_data_with_attributes.attributes)
    {
      OtlpPopulateAttributeUtils::PopulateAttribute(proto_sum_point_data->add_attributes(),
                                                    kv_attr.first, kv_attr.second);
    }
  }
}

void OtlpMetricUtils::ConvertHistogramMetric(
    const metric_sdk::MetricData &metric_data,
    proto::metrics::v1::Histogram *const histogram) noexcept
{
  histogram->set_aggregation_temporality(
      GetProtoAggregationTemporality(metric_data.aggregation_temporality));
  auto start_ts = metric_data.start_ts.time_since_epoch().count();
  auto ts       = metric_data.end_ts.time_since_epoch().count();
  for (auto &point_data_with_attributes : metric_data.point_data_attr_)
  {
    proto::metrics::v1::HistogramDataPoint *proto_histogram_point_data =
        histogram->add_data_points();
    proto_histogram_point_data->set_start_time_unix_nano(start_ts);
    proto_histogram_point_data->set_time_unix_nano(ts);
    auto histogram_data =
        nostd::get<sdk::metrics::HistogramPointData>(point_data_with_attributes.point_data);
    // sum
    if ((nostd::holds_alternative<int64_t>(histogram_data.sum_)))
    {
      // Use static_cast to avoid C4244 in MSVC
      proto_histogram_point_data->set_sum(
          static_cast<double>(nostd::get<int64_t>(histogram_data.sum_)));
    }
    else
    {
      proto_histogram_point_data->set_sum(nostd::get<double>(histogram_data.sum_));
    }
    // count
    proto_histogram_point_data->set_count(histogram_data.count_);
    if (histogram_data.record_min_max_)
    {
      if (nostd::holds_alternative<int64_t>(histogram_data.min_))
      {
        // Use static_cast to avoid C4244 in MSVC
        proto_histogram_point_data->set_min(
            static_cast<double>(nostd::get<int64_t>(histogram_data.min_)));
      }
      else
      {
        proto_histogram_point_data->set_min(nostd::get<double>(histogram_data.min_));
      }
      if (nostd::holds_alternative<int64_t>(histogram_data.max_))
      {
        // Use static_cast to avoid C4244 in MSVC
        proto_histogram_point_data->set_max(
            static_cast<double>(nostd::get<int64_t>(histogram_data.max_)));
      }
      else
      {
        proto_histogram_point_data->set_max(nostd::get<double>(histogram_data.max_));
      }
    }
    // buckets

    for (auto bound : histogram_data.boundaries_)
    {
      proto_histogram_point_data->add_explicit_bounds(bound);
    }
    // bucket counts
    for (auto bucket_value : histogram_data.counts_)
    {
      proto_histogram_point_data->add_bucket_counts(bucket_value);
    }
    // attributes
    for (auto &kv_attr : point_data_with_attributes.attributes)
    {
      OtlpPopulateAttributeUtils::PopulateAttribute(proto_histogram_point_data->add_attributes(),
                                                    kv_attr.first, kv_attr.second);
    }
  }
}

void OtlpMetricUtils::ConvertGaugeMetric(const opentelemetry::sdk::metrics::MetricData &metric_data,
                                         proto::metrics::v1::Gauge *const gauge) noexcept
{
  auto start_ts = metric_data.start_ts.time_since_epoch().count();
  auto ts       = metric_data.end_ts.time_since_epoch().count();
  for (auto &point_data_with_attributes : metric_data.point_data_attr_)
  {
    proto::metrics::v1::NumberDataPoint *proto_gauge_point_data = gauge->add_data_points();
    proto_gauge_point_data->set_start_time_unix_nano(start_ts);
    proto_gauge_point_data->set_time_unix_nano(ts);
    auto gauge_data =
        nostd::get<sdk::metrics::LastValuePointData>(point_data_with_attributes.point_data);

    if ((nostd::holds_alternative<int64_t>(gauge_data.value_)))
    {
      proto_gauge_point_data->set_as_int(nostd::get<int64_t>(gauge_data.value_));
    }
    else
    {
      proto_gauge_point_data->set_as_double(nostd::get<double>(gauge_data.value_));
    }
    // set attributes
    for (auto &kv_attr : point_data_with_attributes.attributes)
    {
      OtlpPopulateAttributeUtils::PopulateAttribute(proto_gauge_point_data->add_attributes(),
                                                    kv_attr.first, kv_attr.second);
    }
  }
}

void OtlpMetricUtils::PopulateInstrumentInfoMetrics(
    const opentelemetry::sdk::metrics::MetricData &metric_data,
    proto::metrics::v1::Metric *metric) noexcept
{
  metric->set_name(metric_data.instrument_descriptor.name_);
  metric->set_description(metric_data.instrument_descriptor.description_);
  metric->set_unit(metric_data.instrument_descriptor.unit_);
  auto kind = GetAggregationType(metric_data);
  switch (kind)
  {
    case metric_sdk::AggregationType::kSum: {
      ConvertSumMetric(metric_data, metric->mutable_sum());
      break;
    }
    case metric_sdk::AggregationType::kHistogram: {
      ConvertHistogramMetric(metric_data, metric->mutable_histogram());
      break;
    }
    case metric_sdk::AggregationType::kLastValue: {
      ConvertGaugeMetric(metric_data, metric->mutable_gauge());
      break;
    }
    default:
      break;
  }
}

void OtlpMetricUtils::PopulateResourceMetrics(
    const opentelemetry::sdk::metrics::ResourceMetrics &data,
    proto::metrics::v1::ResourceMetrics *resource_metrics) noexcept
{
  OtlpPopulateAttributeUtils::PopulateAttribute(resource_metrics->mutable_resource(),
                                                *(data.resource_));

  for (auto &scope_metrics : data.scope_metric_data_)
  {
    if (scope_metrics.scope_ == nullptr)
    {
      continue;
    }
    auto scope_lib_metrics                         = resource_metrics->add_scope_metrics();
    proto::common::v1::InstrumentationScope *scope = scope_lib_metrics->mutable_scope();
    scope->set_name(scope_metrics.scope_->GetName());
    scope->set_version(scope_metrics.scope_->GetVersion());
    resource_metrics->set_schema_url(scope_metrics.scope_->GetSchemaURL());

    for (auto &metric_data : scope_metrics.metric_data_)
    {
      PopulateInstrumentInfoMetrics(metric_data, scope_lib_metrics->add_metrics());
    }
  }
}

void OtlpMetricUtils::PopulateRequest(
    const opentelemetry::sdk::metrics::ResourceMetrics &data,
    proto::collector::metrics::v1::ExportMetricsServiceRequest *request) noexcept
{
  if (request == nullptr || data.resource_ == nullptr)
  {
    return;
  }

  auto resource_metrics = request->add_resource_metrics();
  PopulateResourceMetrics(data, resource_metrics);
}

sdk::metrics::AggregationTemporalitySelector OtlpMetricUtils::ChooseTemporalitySelector(
    PreferredAggregationTemporality preferred_aggregation_temporality) noexcept
{
  if (preferred_aggregation_temporality == PreferredAggregationTemporality::kDelta)
  {
    return DeltaTemporalitySelector;
  }
  else if (preferred_aggregation_temporality == PreferredAggregationTemporality::kCumulative)
  {
    return CumulativeTemporalitySelector;
  }
  return LowMemoryTemporalitySelector;
}

sdk::metrics::AggregationTemporality OtlpMetricUtils::DeltaTemporalitySelector(
    sdk::metrics::InstrumentType instrument_type) noexcept
{
  switch (instrument_type)
  {
    case sdk::metrics::InstrumentType::kCounter:
    case sdk::metrics::InstrumentType::kObservableCounter:
    case sdk::metrics::InstrumentType::kHistogram:
    case sdk::metrics::InstrumentType::kObservableGauge:
      return sdk::metrics::AggregationTemporality::kDelta;
    case sdk::metrics::InstrumentType::kUpDownCounter:
    case sdk::metrics::InstrumentType::kObservableUpDownCounter:
      return sdk::metrics::AggregationTemporality::kCumulative;
  }
  return sdk::metrics::AggregationTemporality::kUnspecified;
}

sdk::metrics::AggregationTemporality OtlpMetricUtils::CumulativeTemporalitySelector(
    sdk::metrics::InstrumentType /* instrument_type */) noexcept
{
  return sdk::metrics::AggregationTemporality::kCumulative;
}

sdk::metrics::AggregationTemporality OtlpMetricUtils::LowMemoryTemporalitySelector(
    sdk::metrics::InstrumentType instrument_type) noexcept
{
  switch (instrument_type)
  {
    case sdk::metrics::InstrumentType::kCounter:
    case sdk::metrics::InstrumentType::kHistogram:
      return sdk::metrics::AggregationTemporality::kDelta;
    case sdk::metrics::InstrumentType::kObservableCounter:
    case sdk::metrics::InstrumentType::kObservableGauge:
    case sdk::metrics::InstrumentType::kUpDownCounter:
    case sdk::metrics::InstrumentType::kObservableUpDownCounter:
      return sdk::metrics::AggregationTemporality::kCumulative;
  }
  return sdk::metrics::AggregationTemporality::kUnspecified;
}
}  // namespace otlp
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE
