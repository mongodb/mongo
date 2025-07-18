// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <stddef.h>
#include <stdint.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <ratio>
#include <thread>
#include <utility>
#include <vector>

#include "opentelemetry/common/timestamp.h"
#include "opentelemetry/nostd/span.h"
#include "opentelemetry/sdk/common/atomic_unique_ptr.h"
#include "opentelemetry/sdk/common/circular_buffer.h"
#include "opentelemetry/sdk/common/circular_buffer_range.h"
#include "opentelemetry/sdk/common/global_log_handler.h"
#include "opentelemetry/sdk/trace/batch_span_processor.h"
#include "opentelemetry/sdk/trace/batch_span_processor_options.h"
#include "opentelemetry/sdk/trace/exporter.h"
#include "opentelemetry/sdk/trace/processor.h"
#include "opentelemetry/sdk/trace/recordable.h"
#include "opentelemetry/trace/span_context.h"
#include "opentelemetry/version.h"

using opentelemetry::sdk::common::AtomicUniquePtr;
using opentelemetry::sdk::common::CircularBufferRange;
using opentelemetry::trace::SpanContext;

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace trace
{

BatchSpanProcessor::BatchSpanProcessor(std::unique_ptr<SpanExporter> &&exporter,
                                       const BatchSpanProcessorOptions &options)
    : exporter_(std::move(exporter)),
      max_queue_size_(options.max_queue_size),
      schedule_delay_millis_(options.schedule_delay_millis),
      max_export_batch_size_(options.max_export_batch_size),
      buffer_(max_queue_size_),
      synchronization_data_(std::make_shared<SynchronizationData>()),
      worker_thread_(&BatchSpanProcessor::DoBackgroundWork, this)
{}

std::unique_ptr<Recordable> BatchSpanProcessor::MakeRecordable() noexcept
{
  return exporter_->MakeRecordable();
}

void BatchSpanProcessor::OnStart(Recordable &, const SpanContext &) noexcept
{
  // no-op
}

void BatchSpanProcessor::OnEnd(std::unique_ptr<Recordable> &&span) noexcept
{
  if (synchronization_data_->is_shutdown.load() == true)
  {
    return;
  }

  if (buffer_.Add(std::move(span)) == false)
  {
    OTEL_INTERNAL_LOG_WARN("BatchSpanProcessor queue is full - dropping span.");
    return;
  }

  // If the queue gets at least half full a preemptive notification is
  // sent to the worker thread to start a new export cycle.
  size_t buffer_size = buffer_.size();
  if (buffer_size >= max_queue_size_ / 2 || buffer_size >= max_export_batch_size_)
  {
    // signal the worker thread
    synchronization_data_->cv.notify_all();
  }
}

bool BatchSpanProcessor::ForceFlush(std::chrono::microseconds timeout) noexcept
{
  if (synchronization_data_->is_shutdown.load() == true)
  {
    return false;
  }

  // Now wait for the worker thread to signal back from the Export method
  std::unique_lock<std::mutex> lk_cv(synchronization_data_->force_flush_cv_m);

  std::uint64_t current_sequence =
      synchronization_data_->force_flush_pending_sequence.fetch_add(1, std::memory_order_release) +
      1;
  synchronization_data_->force_flush_timeout_us.store(timeout.count(), std::memory_order_release);
  auto break_condition = [this, current_sequence]() {
    if (synchronization_data_->is_shutdown.load() == true)
    {
      return true;
    }

    // Wake up the worker thread.
    if (synchronization_data_->force_flush_pending_sequence.load(std::memory_order_acquire) >
        synchronization_data_->force_flush_notified_sequence.load(std::memory_order_acquire))
    {
      synchronization_data_->is_force_wakeup_background_worker.store(true,
                                                                     std::memory_order_release);
      synchronization_data_->cv.notify_all();
    }

    return synchronization_data_->force_flush_notified_sequence.load(std::memory_order_acquire) >=
           current_sequence;
  };

  // Fix timeout to meet requirement of wait_for
  timeout = opentelemetry::common::DurationUtil::AdjustWaitForTimeout(
      timeout, std::chrono::microseconds::zero());
  std::chrono::steady_clock::duration timeout_steady =
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(timeout);
  if (timeout_steady <= std::chrono::steady_clock::duration::zero())
  {
    timeout_steady = (std::chrono::steady_clock::duration::max)();
  }

  bool result = false;
  while (!result && timeout_steady > std::chrono::steady_clock::duration::zero())
  {
    // When force_flush_notified_sequence.compare_exchange_strong(...) and
    // force_flush_cv.notify_all() is called between force_flush_pending_sequence.load(...) and
    // force_flush_cv.wait(). We must not wait for ever
    std::chrono::steady_clock::time_point start_timepoint = std::chrono::steady_clock::now();

    std::chrono::microseconds wait_timeout = schedule_delay_millis_;

    if (wait_timeout > timeout_steady)
    {
      wait_timeout = std::chrono::duration_cast<std::chrono::microseconds>(timeout_steady);
    }
    result = synchronization_data_->force_flush_cv.wait_for(lk_cv, wait_timeout, break_condition);
    timeout_steady -= std::chrono::steady_clock::now() - start_timepoint;
  }

  return synchronization_data_->force_flush_notified_sequence.load(std::memory_order_acquire) >=
         current_sequence;
}

void BatchSpanProcessor::DoBackgroundWork()
{
  auto timeout = schedule_delay_millis_;

  while (true)
  {
    // Wait for `timeout` milliseconds
    std::unique_lock<std::mutex> lk(synchronization_data_->cv_m);
    synchronization_data_->cv.wait_for(lk, timeout, [this] {
      if (synchronization_data_->is_force_wakeup_background_worker.load(std::memory_order_acquire))
      {
        return true;
      }

      return !buffer_.empty();
    });
    synchronization_data_->is_force_wakeup_background_worker.store(false,
                                                                   std::memory_order_release);

    if (synchronization_data_->is_shutdown.load() == true)
    {
      DrainQueue();
      return;
    }

    auto start = std::chrono::steady_clock::now();
    Export();
    auto end      = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // Subtract the duration of this export call from the next `timeout`.
    timeout = schedule_delay_millis_ - duration;
  }
}

void BatchSpanProcessor::Export()
{
  do
  {
    std::vector<std::unique_ptr<Recordable>> spans_arr;
    size_t num_records_to_export;
    std::uint64_t notify_force_flush =
        synchronization_data_->force_flush_pending_sequence.load(std::memory_order_acquire);
    if (notify_force_flush)
    {
      num_records_to_export = buffer_.size();
    }
    else
    {
      num_records_to_export =
          buffer_.size() >= max_export_batch_size_ ? max_export_batch_size_ : buffer_.size();
    }

    if (num_records_to_export == 0)
    {
      NotifyCompletion(notify_force_flush, exporter_, synchronization_data_);
      break;
    }

    // Reserve space for the number of records
    spans_arr.reserve(num_records_to_export);

    buffer_.Consume(num_records_to_export,
                    [&](CircularBufferRange<AtomicUniquePtr<Recordable>> range) noexcept {
                      range.ForEach([&](AtomicUniquePtr<Recordable> &ptr) {
                        std::unique_ptr<Recordable> swap_ptr = std::unique_ptr<Recordable>(nullptr);
                        ptr.Swap(swap_ptr);
                        spans_arr.push_back(std::unique_ptr<Recordable>(swap_ptr.release()));
                        return true;
                      });
                    });

    exporter_->Export(nostd::span<std::unique_ptr<Recordable>>(spans_arr.data(), spans_arr.size()));
    NotifyCompletion(notify_force_flush, exporter_, synchronization_data_);
  } while (true);
}

void BatchSpanProcessor::NotifyCompletion(
    std::uint64_t notify_force_flush,
    const std::unique_ptr<SpanExporter> &exporter,
    const std::shared_ptr<SynchronizationData> &synchronization_data)
{
  if (!synchronization_data)
  {
    return;
  }

  if (notify_force_flush >
      synchronization_data->force_flush_notified_sequence.load(std::memory_order_acquire))
  {
    if (exporter)
    {
      std::chrono::microseconds timeout = opentelemetry::common::DurationUtil::AdjustWaitForTimeout(
          std::chrono::microseconds{
              synchronization_data->force_flush_timeout_us.load(std::memory_order_acquire)},
          std::chrono::microseconds::zero());
      exporter->ForceFlush(timeout);
    }

    std::uint64_t notified_sequence =
        synchronization_data->force_flush_notified_sequence.load(std::memory_order_acquire);
    while (notify_force_flush > notified_sequence)
    {
      synchronization_data->force_flush_notified_sequence.compare_exchange_strong(
          notified_sequence, notify_force_flush, std::memory_order_acq_rel);
      synchronization_data->force_flush_cv.notify_all();
    }
  }
}

void BatchSpanProcessor::DrainQueue()
{
  while (true)
  {
    if (buffer_.empty() &&
        synchronization_data_->force_flush_pending_sequence.load(std::memory_order_acquire) <=
            synchronization_data_->force_flush_notified_sequence.load(std::memory_order_acquire))
    {
      break;
    }

    Export();
  }
}

void BatchSpanProcessor::GetWaitAdjustedTime(
    std::chrono::microseconds &timeout,
    std::chrono::time_point<std::chrono::system_clock> &start_time)
{
  auto end_time = std::chrono::system_clock::now();
  auto offset   = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
  start_time    = end_time;
  timeout       = opentelemetry::common::DurationUtil::AdjustWaitForTimeout(
      timeout, std::chrono::microseconds::zero());
  if (timeout > offset && timeout > std::chrono::microseconds::zero())
  {
    timeout -= offset;
  }
  else
  {
    // Some module use zero as indefinite timeout.So we can not reset timeout to zero here
    timeout = std::chrono::microseconds(1);
  }
}

bool BatchSpanProcessor::Shutdown(std::chrono::microseconds timeout) noexcept
{
  auto start_time = std::chrono::system_clock::now();
  std::lock_guard<std::mutex> shutdown_guard{synchronization_data_->shutdown_m};
  bool already_shutdown = synchronization_data_->is_shutdown.exchange(true);

  if (worker_thread_.joinable())
  {
    synchronization_data_->is_force_wakeup_background_worker.store(true, std::memory_order_release);
    synchronization_data_->cv.notify_all();
    worker_thread_.join();
  }

  GetWaitAdjustedTime(timeout, start_time);
  // Should only shutdown exporter ONCE.
  if (!already_shutdown && exporter_ != nullptr)
  {
    return exporter_->Shutdown(timeout);
  }

  return true;
}

BatchSpanProcessor::~BatchSpanProcessor()
{
  if (synchronization_data_->is_shutdown.load() == false)
  {
    Shutdown();
  }
}

}  // namespace trace
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
