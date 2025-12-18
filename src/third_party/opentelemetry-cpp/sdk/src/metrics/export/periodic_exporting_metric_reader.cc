// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <ostream>
#include <ratio>
#include <thread>
#include <utility>

#include "opentelemetry/common/timestamp.h"
#include "opentelemetry/nostd/function_ref.h"
#include "opentelemetry/sdk/common/global_log_handler.h"
#include "opentelemetry/sdk/metrics/export/metric_producer.h"
#include "opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader.h"
#include "opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_options.h"
#include "opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_runtime_options.h"
#include "opentelemetry/sdk/metrics/instruments.h"
#include "opentelemetry/sdk/metrics/push_metric_exporter.h"
#include "opentelemetry/version.h"

#ifdef ENABLE_THREAD_INSTRUMENTATION_PREVIEW
#  include "opentelemetry/sdk/common/thread_instrumentation.h"
#endif /* ENABLE_THREAD_INSTRUMENTATION_PREVIEW */

#if OPENTELEMETRY_HAVE_EXCEPTIONS
#  include <exception>
#endif

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{

PeriodicExportingMetricReader::PeriodicExportingMetricReader(
    std::unique_ptr<PushMetricExporter> exporter,
    const PeriodicExportingMetricReaderOptions &options)
    : exporter_{std::move(exporter)},
      export_interval_millis_{options.export_interval_millis},
      export_timeout_millis_{options.export_timeout_millis},
      worker_thread_instrumentation_(nullptr),
      collect_thread_instrumentation_(nullptr)
{
  if (export_interval_millis_ <= export_timeout_millis_)
  {
    OTEL_INTERNAL_LOG_WARN(
        "[Periodic Exporting Metric Reader] Invalid configuration: "
        "export_timeout_millis_ should be less than export_interval_millis_, using default values");
    export_interval_millis_ = kExportIntervalMillis;
    export_timeout_millis_  = kExportTimeOutMillis;
  }
}

PeriodicExportingMetricReader::PeriodicExportingMetricReader(
    std::unique_ptr<PushMetricExporter> exporter,
    const PeriodicExportingMetricReaderOptions &options,
    const PeriodicExportingMetricReaderRuntimeOptions &runtime_options)
    : exporter_{std::move(exporter)},
      export_interval_millis_{options.export_interval_millis},
      export_timeout_millis_{options.export_timeout_millis},
      worker_thread_instrumentation_(runtime_options.periodic_thread_instrumentation),
      collect_thread_instrumentation_(runtime_options.collect_thread_instrumentation)
{
  if (export_interval_millis_ <= export_timeout_millis_)
  {
    OTEL_INTERNAL_LOG_WARN(
        "[Periodic Exporting Metric Reader] Invalid configuration: "
        "export_timeout_millis_ should be less than export_interval_millis_, using default values");
    export_interval_millis_ = kExportIntervalMillis;
    export_timeout_millis_  = kExportTimeOutMillis;
  }
}

AggregationTemporality PeriodicExportingMetricReader::GetAggregationTemporality(
    InstrumentType instrument_type) const noexcept
{
  return exporter_->GetAggregationTemporality(instrument_type);
}
void PeriodicExportingMetricReader::OnInitialized() noexcept
{
  worker_thread_ = std::thread(&PeriodicExportingMetricReader::DoBackgroundWork, this);
}

void PeriodicExportingMetricReader::DoBackgroundWork()
{
#ifdef ENABLE_THREAD_INSTRUMENTATION_PREVIEW
  if (worker_thread_instrumentation_ != nullptr)
  {
    worker_thread_instrumentation_->OnStart();
  }
#endif /* ENABLE_THREAD_INSTRUMENTATION_PREVIEW */
  do
  {
    auto start = std::chrono::steady_clock::now();
#ifdef ENABLE_THREAD_INSTRUMENTATION_PREVIEW
    if (worker_thread_instrumentation_ != nullptr)
    {
      worker_thread_instrumentation_->BeforeLoad();
    }
#endif /* ENABLE_THREAD_INSTRUMENTATION_PREVIEW */

    auto status = CollectAndExportOnce();

#ifdef ENABLE_THREAD_INSTRUMENTATION_PREVIEW
    if (worker_thread_instrumentation_ != nullptr)
    {
      worker_thread_instrumentation_->AfterLoad();
    }
#endif /* ENABLE_THREAD_INSTRUMENTATION_PREVIEW */

    if (!status)
    {
      OTEL_INTERNAL_LOG_ERROR("[Periodic Exporting Metric Reader]  Collect-Export Cycle Failure.")
    }

    auto end            = std::chrono::steady_clock::now();
    auto export_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    auto remaining_wait_interval_ms = export_interval_millis_ - export_time_ms;

#ifdef ENABLE_THREAD_INSTRUMENTATION_PREVIEW
    if (worker_thread_instrumentation_ != nullptr)
    {
      worker_thread_instrumentation_->BeforeWait();
    }
#endif /* ENABLE_THREAD_INSTRUMENTATION_PREVIEW */
    std::unique_lock<std::mutex> lk(cv_m_);
    cv_.wait_for(lk, remaining_wait_interval_ms, [this]() {
      if (is_force_wakeup_background_worker_.load(std::memory_order_acquire))
      {
        is_force_wakeup_background_worker_.store(false, std::memory_order_release);
        return true;
      }
      return IsShutdown();
    });

#ifdef ENABLE_THREAD_INSTRUMENTATION_PREVIEW
    if (worker_thread_instrumentation_ != nullptr)
    {
      worker_thread_instrumentation_->AfterWait();
    }
#endif /* ENABLE_THREAD_INSTRUMENTATION_PREVIEW */
  } while (IsShutdown() != true);

#ifdef ENABLE_THREAD_INSTRUMENTATION_PREVIEW
  if (worker_thread_instrumentation_ != nullptr)
  {
    worker_thread_instrumentation_->OnEnd();
  }
#endif /* ENABLE_THREAD_INSTRUMENTATION_PREVIEW */
}

bool PeriodicExportingMetricReader::CollectAndExportOnce()
{
  std::uint64_t notify_force_flush = force_flush_pending_sequence_.load(std::memory_order_acquire);
#if OPENTELEMETRY_HAVE_EXCEPTIONS
  try
  {
#endif
#ifdef ENABLE_THREAD_INSTRUMENTATION_PREVIEW
    if (collect_thread_instrumentation_ != nullptr)
    {
      collect_thread_instrumentation_->OnStart();
      collect_thread_instrumentation_->BeforeLoad();
    }
#endif /* ENABLE_THREAD_INSTRUMENTATION_PREVIEW */
    auto start = std::chrono::steady_clock::now();
    this->Collect([this, &start](ResourceMetrics &metric_data) {
      auto end = std::chrono::steady_clock::now();
      if ((end - start) > this->export_timeout_millis_)
      {
        OTEL_INTERNAL_LOG_ERROR(
            "[Periodic Exporting Metric Reader] Collect took longer configured time: "
            << this->export_timeout_millis_.count() << " ms, and timed out");
        return false;
      }
      this->exporter_->Export(metric_data);
      return true;
    });

#ifdef ENABLE_THREAD_INSTRUMENTATION_PREVIEW
    if (collect_thread_instrumentation_ != nullptr)
    {
      collect_thread_instrumentation_->AfterLoad();
      collect_thread_instrumentation_->OnEnd();
    }
#endif /* ENABLE_THREAD_INSTRUMENTATION_PREVIEW */
#if OPENTELEMETRY_HAVE_EXCEPTIONS
  }
  catch (std::exception &e)
  {
    OTEL_INTERNAL_LOG_ERROR("[Periodic Exporting Metric Reader] Collect failed with exception "
                            << e.what());
    return false;
  }
  catch (...)
  {
    OTEL_INTERNAL_LOG_ERROR(
        "[Periodic Exporting Metric Reader] Collect failed with unknown exception");
    return false;
  }
#endif

  std::uint64_t notified_sequence = force_flush_notified_sequence_.load(std::memory_order_acquire);
  while (notify_force_flush > notified_sequence)
  {
    force_flush_notified_sequence_.compare_exchange_strong(notified_sequence, notify_force_flush,
                                                           std::memory_order_acq_rel);
    force_flush_cv_.notify_all();
  }

  return true;
}

bool PeriodicExportingMetricReader::OnForceFlush(std::chrono::microseconds timeout) noexcept
{
  std::unique_lock<std::mutex> lk_cv(force_flush_m_);
  std::uint64_t current_sequence =
      force_flush_pending_sequence_.fetch_add(1, std::memory_order_release) + 1;
  auto break_condition = [this, current_sequence]() {
    if (IsShutdown())
    {
      return true;
    }

    // Wake up the worker thread.
    if (force_flush_pending_sequence_.load(std::memory_order_acquire) >
        force_flush_notified_sequence_.load(std::memory_order_acquire))
    {
      is_force_wakeup_background_worker_.store(true, std::memory_order_release);
      cv_.notify_all();
    }
    return force_flush_notified_sequence_.load(std::memory_order_acquire) >= current_sequence;
  };

  std::chrono::microseconds wait_timeout =
      opentelemetry::common::DurationUtil::AdjustWaitForTimeout(timeout,
                                                                std::chrono::microseconds::zero());
  std::chrono::steady_clock::duration timeout_steady =
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(wait_timeout);
  if (timeout_steady <= std::chrono::steady_clock::duration::zero())
  {
    timeout_steady = (std::chrono::steady_clock::duration::max)();
  }

  bool result = false;
  while (!result && timeout_steady > std::chrono::steady_clock::duration::zero())
  {
    // When force_flush_notified_sequence_.compare_exchange_strong(...) and
    // force_flush_cv_.notify_all() is called between force_flush_pending_sequence_.load(...) and
    // force_flush_cv_.wait(). We must not wait for ever
    std::chrono::steady_clock::time_point start_timepoint = std::chrono::steady_clock::now();

    wait_timeout = export_interval_millis_;
    if (wait_timeout > timeout_steady)
    {
      wait_timeout = std::chrono::duration_cast<std::chrono::microseconds>(timeout_steady);
    }
    result = force_flush_cv_.wait_for(lk_cv, wait_timeout, break_condition);
    timeout_steady -= std::chrono::steady_clock::now() - start_timepoint;
  }

  if (result)
  {
    // - If original `timeout` is `zero`, use that in exporter::forceflush
    // - Else if remaining `timeout_steady` more than zero, use that in exporter::forceflush
    // - Else don't invoke exporter::forceflush ( as remaining time is zero or less)
    if (timeout <= std::chrono::milliseconds::duration::zero())
    {
      result =
          exporter_->ForceFlush(std::chrono::duration_cast<std::chrono::microseconds>(timeout));
    }
    else if (timeout_steady > std::chrono::steady_clock::duration::zero())
    {
      result = exporter_->ForceFlush(
          std::chrono::duration_cast<std::chrono::microseconds>(timeout_steady));
    }
    else
    {
      // remaining timeout_steady is zero or less
      result = false;
    }
  }
  return result &&
         force_flush_notified_sequence_.load(std::memory_order_acquire) >= current_sequence;
}

bool PeriodicExportingMetricReader::OnShutDown(std::chrono::microseconds timeout) noexcept
{
  if (worker_thread_.joinable())
  {
    cv_.notify_all();
    worker_thread_.join();
  }
  return exporter_->Shutdown(timeout);
}

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
