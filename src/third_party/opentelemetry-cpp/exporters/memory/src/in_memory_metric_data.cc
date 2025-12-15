// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include "opentelemetry/exporters/memory/in_memory_metric_data.h"
#include "opentelemetry/sdk/instrumentationscope/instrumentation_scope.h"
#include "opentelemetry/sdk/metrics/export/metric_producer.h"

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
        data_[{scope, metric}].insert({pda.attributes, pda.point_data});
      }
    }
  }
}

const SimpleAggregateInMemoryMetricData::AttributeToPoint &SimpleAggregateInMemoryMetricData::Get(
    const std::string &scope,
    const std::string &metric)
{
  return data_[{scope, metric}];
}

void SimpleAggregateInMemoryMetricData::Clear()
{
  data_.clear();
}

}  // namespace memory
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE
