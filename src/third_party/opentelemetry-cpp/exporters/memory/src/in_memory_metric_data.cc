// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <stddef.h>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "opentelemetry/exporters/memory/in_memory_data.h"
#include "opentelemetry/exporters/memory/in_memory_metric_data.h"
#include "opentelemetry/nostd/variant.h"
#include "opentelemetry/sdk/common/attribute_utils.h"
#include "opentelemetry/sdk/instrumentationscope/instrumentation_scope.h"
#include "opentelemetry/sdk/metrics/data/metric_data.h"
#include "opentelemetry/sdk/metrics/export/metric_producer.h"
#include "opentelemetry/sdk/metrics/instruments.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace memory
{
using sdk::metrics::ResourceMetrics;

CircularBufferInMemoryMetricData::CircularBufferInMemoryMetricData(size_t buffer_size)
    : InMemoryData(buffer_size)
{}

void CircularBufferInMemoryMetricData::Add(std::unique_ptr<ResourceMetrics> resource_metrics)
{
  InMemoryData::Add(std::move(resource_metrics));
}

void SimpleAggregateInMemoryMetricData::Add(std::unique_ptr<ResourceMetrics> resource_metrics)
{
  for (const auto &sm : resource_metrics->scope_metric_data_)
  {
    const auto &scope = sm.scope_->GetName();
    for (const auto &m : sm.metric_data_)
    {
      const auto &metric = m.instrument_descriptor.name_;
      for (const auto &pda : m.point_data_attr_)
      {
        // NOTE: Explicit type conversion added for C++11 (gcc 4.8)
        data_[std::tuple<std::string, std::string>{scope, metric}].insert(
            {pda.attributes, pda.point_data});
      }
    }
  }
}

const SimpleAggregateInMemoryMetricData::AttributeToPoint &SimpleAggregateInMemoryMetricData::Get(
    const std::string &scope,
    const std::string &metric)
{
  // NOTE: Explicit type conversion added for C++11 (gcc 4.8)
  return data_[std::tuple<std::string, std::string>{scope, metric}];
}

void SimpleAggregateInMemoryMetricData::Clear()
{
  data_.clear();
}

}  // namespace memory
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE
