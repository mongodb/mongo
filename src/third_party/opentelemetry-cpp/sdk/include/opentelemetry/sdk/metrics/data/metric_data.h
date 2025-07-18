// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <vector>

#include "opentelemetry/nostd/variant.h"
#include "opentelemetry/sdk/common/attribute_utils.h"
#include "opentelemetry/sdk/metrics/data/point_data.h"
#include "opentelemetry/sdk/metrics/instruments.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{

using PointAttributes = opentelemetry::sdk::common::OrderedAttributeMap;
using PointType       = opentelemetry::nostd::
    variant<SumPointData, HistogramPointData, LastValuePointData, DropPointData>;

struct PointDataAttributes
{
  PointAttributes attributes;
  PointType point_data;
};

class MetricData
{
public:
  InstrumentDescriptor instrument_descriptor;
  AggregationTemporality aggregation_temporality;
  opentelemetry::common::SystemTimestamp start_ts;
  opentelemetry::common::SystemTimestamp end_ts;
  std::vector<PointDataAttributes> point_data_attr_;
};

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
