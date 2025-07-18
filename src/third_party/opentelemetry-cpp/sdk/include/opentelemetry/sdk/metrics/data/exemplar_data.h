// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>

#include "opentelemetry/common/timestamp.h"
#include "opentelemetry/sdk/metrics/data/metric_data.h"
#include "opentelemetry/sdk/metrics/state/filtered_ordered_attribute_map.h"
#include "opentelemetry/trace/span_context.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{
using MetricAttributes = opentelemetry::sdk::metrics::FilteredOrderedAttributeMap;
/**
 * A sample input measurement.
 *
 * Exemplars also hold information about the environment when the measurement was recorded, for
 * example the span and trace ID of the active span when the exemplar was recorded.
 */
class ExemplarData
{
public:
  static ExemplarData Create(std::shared_ptr<opentelemetry::trace::SpanContext> context,
                             const opentelemetry::common::SystemTimestamp &timestamp,
                             const PointDataAttributes &point_data_attr)
  {
    return ExemplarData(context, timestamp, point_data_attr);
  }

  /**
   * The set of key/value pairs that were filtered out by the aggregator, but recorded alongside
   * the original measurement. Only key/value pairs that were filtered out by the aggregator
   * should be included
   */
  MetricAttributes GetFilteredAttributes() { return MetricAttributes{}; }

  /** Returns the timestamp in nanos when measurement was collected. */
  opentelemetry::common::SystemTimestamp GetEpochNanos() { return timestamp_; }

  /**
   * Returns the SpanContext associated with this exemplar. If the exemplar was not recorded
   * inside a sampled trace, the Context will be invalid.
   */
  const opentelemetry::trace::SpanContext &GetSpanContext() const noexcept { return context_; }

  static PointType CreateSumPointData(ValueType value)
  {
    SumPointData sum_point_data{};
    sum_point_data.value_ = value;
    return sum_point_data;
  }

  static PointType CreateLastValuePointData(ValueType value)
  {
    LastValuePointData last_value_point_data{};
    last_value_point_data.value_              = value;
    last_value_point_data.is_lastvalue_valid_ = true;
    last_value_point_data.sample_ts_          = opentelemetry::common::SystemTimestamp{};
    return last_value_point_data;
  }

  static PointType CreateDropPointData() { return DropPointData{}; }

private:
  ExemplarData(std::shared_ptr<opentelemetry::trace::SpanContext> context,
               opentelemetry::common::SystemTimestamp timestamp,
               const PointDataAttributes &point_data_attr)
      : context_(*context.get()), timestamp_(timestamp), point_data_attr_(point_data_attr)
  {}

  opentelemetry::trace::SpanContext context_;
  opentelemetry::common::SystemTimestamp timestamp_;
  PointDataAttributes point_data_attr_;
};

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
