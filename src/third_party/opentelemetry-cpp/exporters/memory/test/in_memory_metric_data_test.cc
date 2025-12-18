// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "opentelemetry/exporters/memory/in_memory_metric_data.h"
#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/nostd/unique_ptr.h"
#include "opentelemetry/nostd/variant.h"
#include "opentelemetry/sdk/instrumentationscope/instrumentation_scope.h"
#include "opentelemetry/sdk/metrics/data/metric_data.h"
#include "opentelemetry/sdk/metrics/data/point_data.h"
#include "opentelemetry/sdk/metrics/export/metric_producer.h"
#include "opentelemetry/sdk/metrics/instruments.h"
#include "opentelemetry/sdk/resource/resource.h"

using opentelemetry::exporter::memory::CircularBufferInMemoryMetricData;
using opentelemetry::exporter::memory::SimpleAggregateInMemoryMetricData;
using opentelemetry::sdk::metrics::MetricData;
using opentelemetry::sdk::metrics::PointDataAttributes;
using opentelemetry::sdk::metrics::ResourceMetrics;
using opentelemetry::sdk::metrics::ScopeMetrics;
using opentelemetry::sdk::metrics::SumPointData;
using opentelemetry::sdk::resource::Resource;

TEST(InMemoryMetricDataTest, CircularBuffer)
{
  CircularBufferInMemoryMetricData buf(10);
  Resource resource = Resource::GetEmpty();
  buf.Add(std::unique_ptr<ResourceMetrics>(new ResourceMetrics{
      &resource, std::vector<ScopeMetrics>{{nullptr, std::vector<MetricData>{}}}}));
  EXPECT_EQ((*buf.Get().begin())->resource_, &resource);
}

TEST(InMemoryMetricDataTest, SimpleAggregate)
{
  SimpleAggregateInMemoryMetricData agg;

  Resource resource = Resource::GetEmpty();

  auto scope = opentelemetry::sdk::instrumentationscope::InstrumentationScope::Create(
      "my-scope", "1.0.0", "http://example.com");

  SumPointData spd;
  spd.value_ = 42.0;
  PointDataAttributes pda{{{"hello", "world"}}, spd};

  MetricData md;
  md.instrument_descriptor.name_ = "my-metric";
  md.point_data_attr_.push_back(pda);

  agg.Add(std::unique_ptr<ResourceMetrics>(new ResourceMetrics{
      &resource, std::vector<ScopeMetrics>{{scope.get(), std::vector<MetricData>{md}}}}));
  auto it = agg.Get("my-scope", "my-metric").begin();

  auto saved_point = opentelemetry::nostd::get<SumPointData>(it->second);

  EXPECT_EQ(opentelemetry::nostd::get<double>(saved_point.value_), 42.0);
}
