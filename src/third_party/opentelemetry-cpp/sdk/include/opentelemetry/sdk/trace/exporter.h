// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <memory>

#include "opentelemetry/nostd/span.h"
#include "opentelemetry/sdk/common/exporter_utils.h"
#include "opentelemetry/sdk/trace/recordable.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace trace
{

/**
 * SpanExporter defines the interface that protocol-specific span exporters must
 * implement.
 */
class OPENTELEMETRY_EXPORT SpanExporter
{
public:
  SpanExporter();
  virtual ~SpanExporter();

  /**
   * Create a span recordable. This object will be used to record span data and
   * will subsequently be passed to SpanExporter::Export. Vendors can implement
   * custom recordables or use the default SpanData recordable provided by the
   * SDK.
   * @return a newly initialized Recordable object
   *
   * Note: This method must be callable from multiple threads.
   */
  virtual std::unique_ptr<Recordable> MakeRecordable() noexcept = 0;

  /**
   * Exports a batch of span recordables. This method must not be called
   * concurrently for the same exporter instance.
   * @param spans a span of unique pointers to span recordables
   */
  virtual sdk::common::ExportResult Export(
      const nostd::span<std::unique_ptr<Recordable>> &spans) noexcept = 0;

  /**
   * Export all spans that have been exported.
   * @param timeout an optional timeout, the default timeout of 0 means that no
   * timeout is applied.
   * @return return true when all data are exported, and false when timeout
   */
  virtual bool ForceFlush(
      std::chrono::microseconds timeout = (std::chrono::microseconds::max)()) noexcept;

  /**
   * Shut down the exporter.
   * @param timeout an optional timeout.
   * @return return the status of the operation.
   */
  virtual bool Shutdown(
      std::chrono::microseconds timeout = (std::chrono::microseconds::max)()) noexcept = 0;
};
}  // namespace trace
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
