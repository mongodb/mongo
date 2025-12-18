// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <memory>

#include "opentelemetry/nostd/function_ref.h"
#include "opentelemetry/sdk/metrics/export/metric_filter.h"
#include "opentelemetry/sdk/metrics/export/metric_producer.h"
#include "opentelemetry/sdk/metrics/instruments.h"
#include "opentelemetry/sdk/metrics/metric_reader.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{

class MetricReader;
class MeterContext;

class CollectorHandle
{
public:
  CollectorHandle()          = default;
  virtual ~CollectorHandle() = default;

  virtual AggregationTemporality GetAggregationTemporality(
      InstrumentType instrument_type) noexcept = 0;
};

/**
 * An internal opaque interface that the MetricReader receives as
 * MetricProducer. It acts as the storage key to the internal metric stream
 * state for each MetricReader.
 */

class MetricCollector : public MetricProducer, public CollectorHandle
{
public:
  MetricCollector(MeterContext *context,
                  std::shared_ptr<MetricReader> metric_reader,
                  std::unique_ptr<MetricFilter> metric_filter = nullptr);

  ~MetricCollector() override = default;

  AggregationTemporality GetAggregationTemporality(
      InstrumentType instrument_type) noexcept override;

  /**
   * The callback to be called for each metric exporter. This will only be those
   * metrics that have been produced since the last time this method was called.
   *
   * @return a status of completion of method.
   */
  Result Produce() noexcept override;

  bool ForceFlush(std::chrono::microseconds timeout = (std::chrono::microseconds::max)()) noexcept;

  bool Shutdown(std::chrono::microseconds timeout = (std::chrono::microseconds::max)()) noexcept;

private:
  MeterContext *meter_context_;
  std::shared_ptr<MetricReader> metric_reader_;
  std::unique_ptr<MetricFilter> metric_filter_;
};
}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
