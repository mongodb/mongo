// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

#include "opentelemetry/sdk/common/thread_instrumentation.h"
#include "opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_options.h"
#include "opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_runtime_options.h"
#include "opentelemetry/sdk/metrics/instruments.h"
#include "opentelemetry/sdk/metrics/metric_reader.h"
#include "opentelemetry/sdk/metrics/push_metric_exporter.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{

class PeriodicExportingMetricReader : public MetricReader
{

public:
  PeriodicExportingMetricReader(std::unique_ptr<PushMetricExporter> exporter,
                                const PeriodicExportingMetricReaderOptions &options);

  PeriodicExportingMetricReader(std::unique_ptr<PushMetricExporter> exporter,
                                const PeriodicExportingMetricReaderOptions &options,
                                const PeriodicExportingMetricReaderRuntimeOptions &runtime_options);

  AggregationTemporality GetAggregationTemporality(
      InstrumentType instrument_type) const noexcept override;

private:
  bool OnForceFlush(std::chrono::microseconds timeout) noexcept override;

  bool OnShutDown(std::chrono::microseconds timeout) noexcept override;

  void OnInitialized() noexcept override;

  std::unique_ptr<PushMetricExporter> exporter_;
  std::chrono::milliseconds export_interval_millis_;
  std::chrono::milliseconds export_timeout_millis_;

  void DoBackgroundWork();
  bool CollectAndExportOnce();

  /* Synchronization primitives */
  std::atomic<bool> is_force_wakeup_background_worker_{false};
  std::atomic<uint64_t> force_flush_pending_sequence_{0};
  std::atomic<uint64_t> force_flush_notified_sequence_{0};
  std::condition_variable cv_, force_flush_cv_;
  std::mutex cv_m_, force_flush_m_;

  /* The background worker thread */
  std::shared_ptr<sdk::common::ThreadInstrumentation> worker_thread_instrumentation_;
  std::shared_ptr<sdk::common::ThreadInstrumentation> collect_thread_instrumentation_;
  std::thread worker_thread_;
};

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
