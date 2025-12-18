// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

#include "opentelemetry/exporters/memory/in_memory_metric_data.h"
#include "opentelemetry/exporters/memory/in_memory_metric_exporter_factory.h"
#include "opentelemetry/sdk/common/exporter_utils.h"
#include "opentelemetry/sdk/metrics/export/metric_producer.h"
#include "opentelemetry/sdk/metrics/instruments.h"
#include "opentelemetry/sdk/metrics/push_metric_exporter.h"
#include "opentelemetry/sdk/resource/resource.h"

using opentelemetry::exporter::memory::CircularBufferInMemoryMetricData;
using opentelemetry::exporter::memory::InMemoryMetricExporterFactory;
using opentelemetry::sdk::common::ExportResult;
using opentelemetry::sdk::metrics::AggregationTemporality;
using opentelemetry::sdk::metrics::InstrumentType;
using opentelemetry::sdk::metrics::PushMetricExporter;
using opentelemetry::sdk::metrics::ResourceMetrics;
using opentelemetry::sdk::metrics::ScopeMetrics;
using opentelemetry::sdk::resource::Resource;

class InMemoryMetricExporterTest : public ::testing::Test
{
protected:
  InMemoryMetricExporterTest() { exporter_ = InMemoryMetricExporterFactory::Create(data_); }

  std::unique_ptr<PushMetricExporter> exporter_;
  std::shared_ptr<CircularBufferInMemoryMetricData> data_ =
      std::make_shared<CircularBufferInMemoryMetricData>(10);

  Resource resource_ = Resource::GetEmpty();
  ResourceMetrics resource_metrics_{&resource_, std::vector<ScopeMetrics>{}};
};

TEST_F(InMemoryMetricExporterTest, Export)
{
  EXPECT_EQ(exporter_->Export(resource_metrics_), ExportResult::kSuccess);

  auto data = data_->Get();
  EXPECT_EQ(data.size(), 1);
  EXPECT_EQ((*data.begin())->resource_, &resource_);
}

TEST_F(InMemoryMetricExporterTest, ForceFlush)
{
  EXPECT_TRUE(exporter_->ForceFlush());
}

TEST_F(InMemoryMetricExporterTest, Shutdown)
{
  EXPECT_TRUE(exporter_->Shutdown());
  EXPECT_EQ(exporter_->Export(resource_metrics_), ExportResult::kFailure);
}

TEST_F(InMemoryMetricExporterTest, TemporalitySelector)
{
  EXPECT_EQ(exporter_->GetAggregationTemporality(InstrumentType::kCounter),
            AggregationTemporality::kCumulative);
}
