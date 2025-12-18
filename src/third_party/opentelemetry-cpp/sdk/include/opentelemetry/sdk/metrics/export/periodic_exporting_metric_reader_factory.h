// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>

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

class OPENTELEMETRY_EXPORT PeriodicExportingMetricReaderFactory
{
public:
  static std::unique_ptr<MetricReader> Create(std::unique_ptr<PushMetricExporter> exporter,
                                              const PeriodicExportingMetricReaderOptions &options);

  static std::unique_ptr<MetricReader> Create(
      std::unique_ptr<PushMetricExporter> exporter,
      const PeriodicExportingMetricReaderOptions &options,
      const PeriodicExportingMetricReaderRuntimeOptions &runtime_options);
};

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
