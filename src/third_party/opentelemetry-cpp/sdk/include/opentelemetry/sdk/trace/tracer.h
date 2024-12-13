// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <stdint.h>

#include "opentelemetry/common/key_value_iterable.h"
#include "opentelemetry/nostd/shared_ptr.h"
#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/sdk/instrumentationscope/instrumentation_scope.h"
#include "opentelemetry/sdk/resource/resource.h"
#include "opentelemetry/sdk/trace/id_generator.h"
#include "opentelemetry/sdk/trace/processor.h"
#include "opentelemetry/sdk/trace/sampler.h"
#include "opentelemetry/sdk/trace/tracer_context.h"
#include "opentelemetry/trace/span.h"
#include "opentelemetry/trace/span_context_kv_iterable.h"
#include "opentelemetry/trace/span_startoptions.h"
#include "opentelemetry/trace/tracer.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace trace
{

using namespace opentelemetry::sdk::instrumentationscope;

class Tracer final : public opentelemetry::trace::Tracer,
                     public std::enable_shared_from_this<Tracer>
{
public:
  /** Construct a new Tracer with the given context pipeline. */
  explicit Tracer(std::shared_ptr<TracerContext> context,
                  std::unique_ptr<InstrumentationScope> instrumentation_scope =
                      InstrumentationScope::Create("")) noexcept;

  nostd::shared_ptr<opentelemetry::trace::Span> StartSpan(
      nostd::string_view name,
      const opentelemetry::common::KeyValueIterable &attributes,
      const opentelemetry::trace::SpanContextKeyValueIterable &links,
      const opentelemetry::trace::StartSpanOptions &options = {}) noexcept override;

#if OPENTELEMETRY_ABI_VERSION_NO >= 2
  /**
   * Force any buffered spans to flush.
   * @param timeout to complete the flush
   */
  template <class Rep, class Period>
  void ForceFlush(std::chrono::duration<Rep, Period> timeout) noexcept
  {
    this->ForceFlushWithMicroseconds(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(timeout).count()));
  }

  void ForceFlushWithMicroseconds(uint64_t timeout) noexcept;

  /**
   * ForceFlush any buffered spans and stop reporting spans.
   * @param timeout to complete the flush
   */
  template <class Rep, class Period>
  void Close(std::chrono::duration<Rep, Period> timeout) noexcept
  {
    this->CloseWithMicroseconds(static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(timeout).count()));
  }

  void CloseWithMicroseconds(uint64_t timeout) noexcept;
#else
  /* Exposed in the API in ABI version 1, but does not belong to the API */
  void ForceFlushWithMicroseconds(uint64_t timeout) noexcept override;

  void CloseWithMicroseconds(uint64_t timeout) noexcept override;
#endif

  /** Returns the configured span processor. */
  SpanProcessor &GetProcessor() noexcept { return context_->GetProcessor(); }

  /** Returns the configured Id generator */
  IdGenerator &GetIdGenerator() const noexcept { return context_->GetIdGenerator(); }

  /** Returns the associated instrumentation scope */
  const InstrumentationScope &GetInstrumentationScope() const noexcept
  {
    return *instrumentation_scope_;
  }

  OPENTELEMETRY_DEPRECATED_MESSAGE("Please use GetInstrumentationScope instead")
  const InstrumentationScope &GetInstrumentationLibrary() const noexcept
  {
    return GetInstrumentationScope();
  }

  /** Returns the currently configured resource **/
  const opentelemetry::sdk::resource::Resource &GetResource() { return context_->GetResource(); }

  // Note: Test only
  Sampler &GetSampler() { return context_->GetSampler(); }

private:
  // order of declaration is important here - instrumentation scope should destroy after
  // tracer-context.
  std::shared_ptr<InstrumentationScope> instrumentation_scope_;
  std::shared_ptr<TracerContext> context_;
};
}  // namespace trace
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
