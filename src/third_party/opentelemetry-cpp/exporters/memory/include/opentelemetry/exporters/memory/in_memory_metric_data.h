// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <stddef.h>
#include <map>
#include <memory>
#include <string>
#include <tuple>

#include "opentelemetry/exporters/memory/in_memory_data.h"
#include "opentelemetry/sdk/metrics/data/metric_data.h"
#include "opentelemetry/sdk/metrics/export/metric_producer.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace memory
{

/// The abstract base class for types used to store in-memory data backing an
/// InMemoryMetricExporter.
class InMemoryMetricData
{
public:
  InMemoryMetricData()          = default;
  virtual ~InMemoryMetricData() = default;

  InMemoryMetricData(const InMemoryMetricData &)            = delete;
  InMemoryMetricData(InMemoryMetricData &&)                 = delete;
  InMemoryMetricData &operator=(const InMemoryMetricData &) = delete;
  InMemoryMetricData &operator=(InMemoryMetricData &&)      = delete;

  virtual void Add(std::unique_ptr<sdk::metrics::ResourceMetrics> resource_metrics) = 0;
};

/// An implementation of InMemoryMetricData that stores full-fidelity data points in a circular
/// buffer. This allows tests to inspect every aspect of exported data, in exchange for a somewhat
/// cumbersome API.
class CircularBufferInMemoryMetricData final : public InMemoryMetricData,
                                               public InMemoryData<sdk::metrics::ResourceMetrics>
{
public:
  explicit CircularBufferInMemoryMetricData(size_t buffer_size);
  void Add(std::unique_ptr<sdk::metrics::ResourceMetrics> resource_metrics) override;
};

/// An implementation of InMemoryMetricData that stores only the most recent data point in each time
/// series, and allows convenient lookups of time series. This makes simple tests easier to write.
class SimpleAggregateInMemoryMetricData final : public InMemoryMetricData
{
public:
  using AttributeToPoint = std::map<opentelemetry::sdk::metrics::PointAttributes,
                                    opentelemetry::sdk::metrics::PointType>;

  void Add(std::unique_ptr<sdk::metrics::ResourceMetrics> resource_metrics) override;
  const AttributeToPoint &Get(const std::string &scope, const std::string &metric);
  void Clear();

private:
  std::map<std::tuple<std::string, std::string>, AttributeToPoint> data_;
};

}  // namespace memory
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE
