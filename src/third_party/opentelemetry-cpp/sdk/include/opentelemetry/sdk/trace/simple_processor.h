// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <utility>

#include "opentelemetry/common/spin_lock_mutex.h"
#include "opentelemetry/nostd/span.h"
#include "opentelemetry/sdk/common/exporter_utils.h"
#include "opentelemetry/sdk/trace/exporter.h"
#include "opentelemetry/sdk/trace/processor.h"
#include "opentelemetry/sdk/trace/recordable.h"
#include "opentelemetry/trace/span_context.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace trace
{
/**
 * The simple span processor passes finished recordables to the configured
 * SpanExporter, as soon as they are finished.
 *
 * OnEnd and ForceFlush are no-ops.
 *
 * All calls to the configured SpanExporter are synchronized using a
 * spin-lock on an atomic_flag.
 */
class SimpleSpanProcessor : public SpanProcessor
{
public:
  /**
   * Initialize a simple span processor.
   * @param exporter the exporter used by the span processor
   */
  explicit SimpleSpanProcessor(std::unique_ptr<SpanExporter> &&exporter) noexcept
      : exporter_(std::move(exporter))
  {}

  std::unique_ptr<Recordable> MakeRecordable() noexcept override
  {
    return exporter_->MakeRecordable();
  }

  void OnStart(Recordable & /* span */,
               const opentelemetry::trace::SpanContext & /* parent_context */) noexcept override
  {}

  void OnEnd(std::unique_ptr<Recordable> &&span) noexcept override
  {
    nostd::span<std::unique_ptr<Recordable>> batch(&span, 1);
    const std::lock_guard<opentelemetry::common::SpinLockMutex> locked(lock_);
    if (exporter_->Export(batch) == sdk::common::ExportResult::kFailure)
    {
      /* Once it is defined how the SDK does logging, an error should be
       * logged in this case. */
    }
  }

  bool ForceFlush(
      std::chrono::microseconds timeout = (std::chrono::microseconds::max)()) noexcept override
  {
    if (exporter_ != nullptr)
    {
      return exporter_->ForceFlush(timeout);
    }

    return true;
  }

  bool Shutdown(
      std::chrono::microseconds timeout = (std::chrono::microseconds::max)()) noexcept override
  {
    // We only call shutdown ONCE.
    if (exporter_ != nullptr && !shutdown_latch_.test_and_set(std::memory_order_acquire))
    {
      return exporter_->Shutdown(timeout);
    }
    return true;
  }

  ~SimpleSpanProcessor() override { Shutdown(); }

private:
  std::unique_ptr<SpanExporter> exporter_;
  opentelemetry::common::SpinLockMutex lock_;
#if defined(__cpp_lib_atomic_value_initialization) && \
    __cpp_lib_atomic_value_initialization >= 201911L
  std::atomic_flag shutdown_latch_{};
#else
  std::atomic_flag shutdown_latch_ = ATOMIC_FLAG_INIT;
#endif
};
}  // namespace trace
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
