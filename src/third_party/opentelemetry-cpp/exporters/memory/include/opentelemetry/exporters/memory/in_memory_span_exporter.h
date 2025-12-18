// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <stddef.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <ostream>
#include <utility>

#include "opentelemetry/exporters/memory/in_memory_span_data.h"
#include "opentelemetry/nostd/span.h"
#include "opentelemetry/sdk/common/exporter_utils.h"
#include "opentelemetry/sdk/common/global_log_handler.h"
#include "opentelemetry/sdk/trace/exporter.h"
#include "opentelemetry/sdk/trace/recordable.h"
#include "opentelemetry/sdk/trace/span_data.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace memory
{
const size_t MAX_BUFFER_SIZE = 100;

/**
 * A in memory exporter that switches a flag once a valid recordable was received
 * and keeps track of all received spans in memory.
 */
class InMemorySpanExporter final : public opentelemetry::sdk::trace::SpanExporter
{
public:
  /**
   * @param buffer_size an optional value that sets the size of the InMemorySpanData
   */
  InMemorySpanExporter(size_t buffer_size = MAX_BUFFER_SIZE)
      : data_(new InMemorySpanData(buffer_size))
  {}

  /**
   * @return Returns a unique pointer to an empty recordable object
   */
  std::unique_ptr<sdk::trace::Recordable> MakeRecordable() noexcept override
  {
    return std::unique_ptr<sdk::trace::Recordable>(new sdk::trace::SpanData());
  }

  /**
   * @param recordables a required span containing unique pointers to the data
   * to add to the InMemorySpanData
   * @return Returns the result of the operation
   */
  sdk::common::ExportResult Export(
      const nostd::span<std::unique_ptr<sdk::trace::Recordable>> &recordables) noexcept override
  {
    if (isShutdown())
    {
      OTEL_INTERNAL_LOG_ERROR("[In Memory Span Exporter] Exporting "
                              << recordables.size() << " span(s) failed, exporter is shutdown");
      return sdk::common::ExportResult::kFailure;
    }
    for (auto &recordable : recordables)
    {
      auto span = std::unique_ptr<sdk::trace::SpanData>(
          static_cast<sdk::trace::SpanData *>(recordable.release()));
      if (span != nullptr)
      {
        data_->Add(std::move(span));
      }
    }

    return sdk::common::ExportResult::kSuccess;
  }

  virtual bool ForceFlush(std::chrono::microseconds /* timeout */) noexcept override
  {
    return true;
  }

  /**
   * Attempt to shut down the in-memory span exporter.
   * @param timeout Timeout is an optional value containing the timeout of the exporter
   * note: passing custom timeout values is not currently supported for this exporter
   * @return Returns the status of the operation
   */
  bool Shutdown(std::chrono::microseconds timeout OPENTELEMETRY_MAYBE_UNUSED) noexcept override
  {
    is_shutdown_ = true;
    return true;
  }

  /**
   * @return Returns a shared pointer to this exporters InMemorySpanData
   */
  std::shared_ptr<InMemorySpanData> GetData() noexcept { return data_; }

private:
  std::shared_ptr<InMemorySpanData> data_;
  std::atomic<bool> is_shutdown_{false};
  bool isShutdown() const noexcept { return is_shutdown_; }
};
}  // namespace memory
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE
