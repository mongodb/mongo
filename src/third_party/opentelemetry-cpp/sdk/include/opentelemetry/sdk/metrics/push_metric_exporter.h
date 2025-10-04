// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>

#include "opentelemetry/sdk/common/exporter_utils.h"
#include "opentelemetry/sdk/metrics/instruments.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{
class MetricData;
struct ResourceMetrics;

/**
 * PushMetricExporter defines the interface to be used by metrics libraries to
 *  push metrics data to the OpenTelemetry exporters.
 */
class PushMetricExporter
{
public:
  virtual ~PushMetricExporter() = default;

  /**
   * Exports a batch of metrics data. This method must not be called
   * concurrently for the same exporter instance.
   * @param data metrics data
   */
  virtual opentelemetry::sdk::common::ExportResult Export(const ResourceMetrics &data) noexcept = 0;

  /**
   * Get the AggregationTemporality for given Instrument Type for this exporter.
   *
   * @return AggregationTemporality
   */
  virtual AggregationTemporality GetAggregationTemporality(
      InstrumentType instrument_type) const noexcept = 0;

  /**
   * Force flush the exporter.
   */
  virtual bool ForceFlush(
      std::chrono::microseconds timeout = (std::chrono::microseconds::max)()) noexcept = 0;

  /**
   * Shut down the metric exporter.
   * @param timeout an optional timeout.
   * @return return the status of the operation.
   */
  virtual bool Shutdown(
      std::chrono::microseconds timeout = std::chrono::microseconds(0)) noexcept = 0;
};
}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
