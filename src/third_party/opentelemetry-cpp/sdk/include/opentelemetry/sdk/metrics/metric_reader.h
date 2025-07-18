// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <chrono>

#include "opentelemetry/nostd/function_ref.h"
#include "opentelemetry/sdk/metrics/export/metric_producer.h"
#include "opentelemetry/sdk/metrics/instruments.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{
/**
 * MetricReader defines the interface to collect metrics from SDK
 */
class MetricReader
{
public:
  MetricReader();

  void SetMetricProducer(MetricProducer *metric_producer);

  /**
   * Collect the metrics from SDK.
   * @return return the status of the operation.
   */
  bool Collect(nostd::function_ref<bool(ResourceMetrics &metric_data)> callback) noexcept;

  /**
   * Get the AggregationTemporality for given Instrument Type for this reader.
   *
   * @return AggregationTemporality
   */

  virtual AggregationTemporality GetAggregationTemporality(
      InstrumentType instrument_type) const noexcept = 0;

  /**
   * Shutdown the metric reader.
   */
  bool Shutdown(std::chrono::microseconds timeout = (std::chrono::microseconds::max)()) noexcept;

  /**
   * Force flush the metric read by the reader.
   */
  bool ForceFlush(std::chrono::microseconds timeout = (std::chrono::microseconds::max)()) noexcept;

  /**
   * Return the status of Metric reader.
   */
  bool IsShutdown() const noexcept;

  virtual ~MetricReader() = default;

private:
  virtual bool OnForceFlush(std::chrono::microseconds timeout) noexcept = 0;

  virtual bool OnShutDown(std::chrono::microseconds timeout) noexcept = 0;

  virtual void OnInitialized() noexcept {}

protected:
private:
  MetricProducer *metric_producer_;
  std::atomic<bool> shutdown_{false};
};
}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
