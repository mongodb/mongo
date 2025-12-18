// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <memory>
#include <utility>

#include "opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader.h"
#include "opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_factory.h"
#include "opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_options.h"
#include "opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_runtime_options.h"
#include "opentelemetry/sdk/metrics/metric_reader.h"
#include "opentelemetry/sdk/metrics/push_metric_exporter.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{

std::unique_ptr<MetricReader> PeriodicExportingMetricReaderFactory::Create(
    std::unique_ptr<PushMetricExporter> exporter,
    const PeriodicExportingMetricReaderOptions &options)
{
  PeriodicExportingMetricReaderRuntimeOptions runtime_options;
  return Create(std::move(exporter), options, runtime_options);
}

std::unique_ptr<MetricReader> PeriodicExportingMetricReaderFactory::Create(
    std::unique_ptr<PushMetricExporter> exporter,
    const PeriodicExportingMetricReaderOptions &options,
    const PeriodicExportingMetricReaderRuntimeOptions &runtime_options)
{
  std::unique_ptr<MetricReader> reader(
      new PeriodicExportingMetricReader(std::move(exporter), options, runtime_options));
  return reader;
}

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
