// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>

#include "opentelemetry/sdk/metrics/instruments.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{
class PushMetricExporter;
}  // namespace metrics
}  // namespace sdk
namespace exporter
{
namespace memory
{
class InMemoryMetricData;

/// A factory for InMemoryMetricExporter
class InMemoryMetricExporterFactory
{
public:
  /// Create a InMemoryMetricExporter with a default buffer size and aggregation
  /// temporality selector.
  /// @param [out] data the InMemoryMetricData the exporter will write to,
  ///                   for the caller to inspect
  /// @param [in] buffer_size number of entries to save in the circular buffer
  /// @param [in] temporality output temporality as a function of instrument kind
  static std::unique_ptr<opentelemetry::sdk::metrics::PushMetricExporter> Create(
      const std::shared_ptr<InMemoryMetricData> &data,
      const sdk::metrics::AggregationTemporalitySelector &temporality);

  static std::unique_ptr<opentelemetry::sdk::metrics::PushMetricExporter> Create(
      const std::shared_ptr<InMemoryMetricData> &data);
};
}  // namespace memory
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE
