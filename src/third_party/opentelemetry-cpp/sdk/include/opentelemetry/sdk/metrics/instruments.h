// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <functional>
#include <string>

#include "opentelemetry/sdk/metrics/state/filtered_ordered_attribute_map.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{
enum class InstrumentType
{
  kCounter,
  kHistogram,
  kUpDownCounter,
  kObservableCounter,
  kObservableGauge,
  kObservableUpDownCounter
};

enum class InstrumentClass
{
  kSync,
  kAsync
};

enum class InstrumentValueType
{
  kInt,
  kLong,
  kFloat,
  kDouble
};

enum class AggregationType
{
  kDrop,
  kHistogram,
  kLastValue,
  kSum,
  kDefault
};

enum class AggregationTemporality
{
  kUnspecified,
  kDelta,
  kCumulative
};

struct InstrumentDescriptor
{
  std::string name_;
  std::string description_;
  std::string unit_;
  InstrumentType type_;
  InstrumentValueType value_type_;
};

using MetricAttributes               = opentelemetry::sdk::metrics::FilteredOrderedAttributeMap;
using AggregationTemporalitySelector = std::function<AggregationTemporality(InstrumentType)>;

/*class InstrumentSelector {
public:
InstrumentSelector(opentelemetry::nostd::string_view name,
opentelemetry::sdk::metrics::InstrumentType type): name_(name.data()), type_(type) {} InstrumentType
GetType(){return type_;} std::string GetNameFilter() { return name_;}

private:
std::string name_;
InstrumentType type_;
};*/
}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
