// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <stdint.h>
#include <chrono>
#include <map>
#include <new>
#include <utility>

#include "opentelemetry/common/key_value_iterable.h"
#include "opentelemetry/context/context.h"
#include "opentelemetry/nostd/shared_ptr.h"
#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/nostd/variant.h"
#include "opentelemetry/sdk/instrumentationscope/instrumentation_scope.h"
#include "opentelemetry/sdk/trace/id_generator.h"
#include "opentelemetry/sdk/trace/sampler.h"
#include "opentelemetry/sdk/trace/tracer.h"
#include "opentelemetry/sdk/trace/tracer_context.h"
#include "opentelemetry/trace/context.h"
#include "opentelemetry/trace/noop.h"
#include "opentelemetry/trace/span.h"
#include "opentelemetry/trace/span_context.h"
#include "opentelemetry/trace/span_context_kv_iterable.h"
#include "opentelemetry/trace/span_id.h"
#include "opentelemetry/trace/span_startoptions.h"
#include "opentelemetry/trace/trace_flags.h"
#include "opentelemetry/trace/trace_id.h"
#include "opentelemetry/trace/trace_state.h"
#include "opentelemetry/version.h"

#include "src/trace/span.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace trace
{

Tracer::Tracer(std::shared_ptr<TracerContext> context,
               std::unique_ptr<InstrumentationScope> instrumentation_scope) noexcept
    : instrumentation_scope_{std::move(instrumentation_scope)}, context_{std::move(context)}
{}

nostd::shared_ptr<opentelemetry::trace::Span> Tracer::StartSpan(
    nostd::string_view name,
    const opentelemetry::common::KeyValueIterable &attributes,
    const opentelemetry::trace::SpanContextKeyValueIterable &links,
    const opentelemetry::trace::StartSpanOptions &options) noexcept
{
  opentelemetry::trace::SpanContext parent_context = GetCurrentSpan()->GetContext();
  if (nostd::holds_alternative<opentelemetry::trace::SpanContext>(options.parent))
  {
    auto span_context = nostd::get<opentelemetry::trace::SpanContext>(options.parent);
    if (span_context.IsValid())
    {
      parent_context = span_context;
    }
  }
  else if (nostd::holds_alternative<context::Context>(options.parent))
  {
    auto context = nostd::get<context::Context>(options.parent);
    // fetch span context from parent span stored in the context
    auto span_context = opentelemetry::trace::GetSpan(context)->GetContext();
    if (span_context.IsValid())
    {
      parent_context = span_context;
    }
    else
    {
      if (opentelemetry::trace::IsRootSpan(context))
      {
        parent_context = opentelemetry::trace::SpanContext{false, false};
      }
    }
  }

  IdGenerator &generator = GetIdGenerator();
  opentelemetry::trace::TraceId trace_id;
  opentelemetry::trace::SpanId span_id = generator.GenerateSpanId();
  bool is_parent_span_valid            = false;
  uint8_t flags                        = 0;

  if (parent_context.IsValid())
  {
    trace_id             = parent_context.trace_id();
    flags                = parent_context.trace_flags().flags();
    is_parent_span_valid = true;
  }
  else
  {
    trace_id = generator.GenerateTraceId();
    if (generator.IsRandom())
    {
      flags = opentelemetry::trace::TraceFlags::kIsRandom;
    }
  }

  auto sampling_result = context_->GetSampler().ShouldSample(parent_context, trace_id, name,
                                                             options.kind, attributes, links);
  if (sampling_result.IsSampled())
  {
    flags |= opentelemetry::trace::TraceFlags::kIsSampled;
  }

#if 1
  /* https://github.com/open-telemetry/opentelemetry-specification as of v1.29.0 */
  /* Support W3C Trace Context version 1. */
  flags &= opentelemetry::trace::TraceFlags::kAllW3CTraceContext1Flags;
#endif

#if 0
  /* Waiting for https://github.com/open-telemetry/opentelemetry-specification/issues/3411 */
  /* Support W3C Trace Context version 2. */
  flags &= opentelemetry::trace::TraceFlags::kAllW3CTraceContext2Flags;
#endif

  opentelemetry::trace::TraceFlags trace_flags(flags);

  auto span_context =
      std::unique_ptr<opentelemetry::trace::SpanContext>(new opentelemetry::trace::SpanContext(
          trace_id, span_id, trace_flags, false,
          sampling_result.trace_state ? sampling_result.trace_state
          : is_parent_span_valid      ? parent_context.trace_state()
                                      : opentelemetry::trace::TraceState::GetDefault()));

  if (!sampling_result.IsRecording())
  {
    // create no-op span with valid span-context.

    auto noop_span = nostd::shared_ptr<opentelemetry::trace::Span>{
        new (std::nothrow)
            opentelemetry::trace::NoopSpan(this->shared_from_this(), std::move(span_context))};
    return noop_span;
  }
  else
  {

    auto span = nostd::shared_ptr<opentelemetry::trace::Span>{
        new (std::nothrow) Span{this->shared_from_this(), name, attributes, links, options,
                                parent_context, std::move(span_context)}};

    // if the attributes is not nullptr, add attributes to the span.
    if (sampling_result.attributes)
    {
      for (auto &kv : *sampling_result.attributes)
      {
        span->SetAttribute(kv.first, kv.second);
      }
    }

    return span;
  }
}

void Tracer::ForceFlushWithMicroseconds(uint64_t timeout) noexcept
{
  if (context_)
  {
    context_->ForceFlush(
        std::chrono::microseconds{static_cast<std::chrono::microseconds::rep>(timeout)});
  }
}

void Tracer::CloseWithMicroseconds(uint64_t timeout) noexcept
{
  // Trace context is shared by many tracers.So we just call ForceFlush to flush all pending spans
  // and do not  shutdown it.
  if (context_)
  {
    context_->ForceFlush(
        std::chrono::microseconds{static_cast<std::chrono::microseconds::rep>(timeout)});
  }
}
}  // namespace trace
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
