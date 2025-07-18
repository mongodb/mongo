// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <utility>

#include "opentelemetry/sdk/trace/processor.h"
#include "opentelemetry/sdk/trace/recordable.h"
#include "opentelemetry/trace/span_id.h"
#include "opentelemetry/trace/span_metadata.h"
#include "opentelemetry/version.h"
#include "src/trace/span.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace trace
{

using opentelemetry::common::SteadyTimestamp;
using opentelemetry::common::SystemTimestamp;
namespace common = opentelemetry::common;

namespace
{
SystemTimestamp NowOr(const SystemTimestamp &system)
{
  if (system == SystemTimestamp())
  {
    return SystemTimestamp(std::chrono::system_clock::now());
  }
  else
  {
    return system;
  }
}

SteadyTimestamp NowOr(const SteadyTimestamp &steady)
{
  if (steady == SteadyTimestamp())
  {
    return SteadyTimestamp(std::chrono::steady_clock::now());
  }
  else
  {
    return steady;
  }
}
}  // namespace

Span::Span(std::shared_ptr<Tracer> &&tracer,
           nostd::string_view name,
           const common::KeyValueIterable &attributes,
           const opentelemetry::trace::SpanContextKeyValueIterable &links,
           const opentelemetry::trace::StartSpanOptions &options,
           const opentelemetry::trace::SpanContext &parent_span_context,
           std::unique_ptr<opentelemetry::trace::SpanContext> span_context) noexcept
    : tracer_{std::move(tracer)},
      recordable_{tracer_->GetProcessor().MakeRecordable()},
      start_steady_time{options.start_steady_time},
      span_context_(std::move(span_context)),
      has_ended_{false}
{
  if (recordable_ == nullptr)
  {
    return;
  }
  recordable_->SetName(name);
  recordable_->SetInstrumentationScope(tracer_->GetInstrumentationScope());
  recordable_->SetIdentity(*span_context_, parent_span_context.IsValid()
                                               ? parent_span_context.span_id()
                                               : opentelemetry::trace::SpanId());

  recordable_->SetTraceFlags(span_context_->trace_flags());

  attributes.ForEachKeyValue([&](nostd::string_view key, common::AttributeValue value) noexcept {
    recordable_->SetAttribute(key, value);
    return true;
  });

  links.ForEachKeyValue([&](const opentelemetry::trace::SpanContext &span_context,
                            const common::KeyValueIterable &attributes) {
    recordable_->AddLink(span_context, attributes);
    return true;
  });

  recordable_->SetSpanKind(options.kind);
  recordable_->SetStartTime(NowOr(options.start_system_time));
  start_steady_time = NowOr(options.start_steady_time);
  recordable_->SetResource(tracer_->GetResource());
  tracer_->GetProcessor().OnStart(*recordable_, parent_span_context);
}

Span::~Span()
{
  End();
}

void Span::SetAttribute(nostd::string_view key, const common::AttributeValue &value) noexcept
{
  std::lock_guard<std::mutex> lock_guard{mu_};
  if (recordable_ == nullptr)
  {
    return;
  }

  recordable_->SetAttribute(key, value);
}

void Span::AddEvent(nostd::string_view name) noexcept
{
  std::lock_guard<std::mutex> lock_guard{mu_};
  if (recordable_ == nullptr)
  {
    return;
  }
  recordable_->AddEvent(name);
}

void Span::AddEvent(nostd::string_view name, SystemTimestamp timestamp) noexcept
{
  std::lock_guard<std::mutex> lock_guard{mu_};
  if (recordable_ == nullptr)
  {
    return;
  }
  recordable_->AddEvent(name, timestamp);
}

void Span::AddEvent(nostd::string_view name, const common::KeyValueIterable &attributes) noexcept
{
  std::lock_guard<std::mutex> lock_guard{mu_};
  if (recordable_ == nullptr)
  {
    return;
  }
  recordable_->AddEvent(name, attributes);
}

void Span::AddEvent(nostd::string_view name,
                    SystemTimestamp timestamp,
                    const common::KeyValueIterable &attributes) noexcept
{
  std::lock_guard<std::mutex> lock_guard{mu_};
  if (recordable_ == nullptr)
  {
    return;
  }
  recordable_->AddEvent(name, timestamp, attributes);
}

#if OPENTELEMETRY_ABI_VERSION_NO >= 2
void Span::AddLink(const opentelemetry::trace::SpanContext &target,
                   const opentelemetry::common::KeyValueIterable &attrs) noexcept
{
  std::lock_guard<std::mutex> lock_guard{mu_};
  if (recordable_ == nullptr)
  {
    return;
  }

  recordable_->AddLink(target, attrs);
}

void Span::AddLinks(const opentelemetry::trace::SpanContextKeyValueIterable &links) noexcept
{
  std::lock_guard<std::mutex> lock_guard{mu_};
  if (recordable_ == nullptr)
  {
    return;
  }

  links.ForEachKeyValue([&](opentelemetry::trace::SpanContext span_context,
                            const common::KeyValueIterable &attributes) {
    recordable_->AddLink(span_context, attributes);
    return true;
  });
}
#endif

void Span::SetStatus(opentelemetry::trace::StatusCode code, nostd::string_view description) noexcept
{
  std::lock_guard<std::mutex> lock_guard{mu_};
  if (recordable_ == nullptr)
  {
    return;
  }
  recordable_->SetStatus(code, description);
}

void Span::UpdateName(nostd::string_view name) noexcept
{
  std::lock_guard<std::mutex> lock_guard{mu_};
  if (recordable_ == nullptr)
  {
    return;
  }
  recordable_->SetName(name);
}

void Span::End(const opentelemetry::trace::EndSpanOptions &options) noexcept
{
  std::lock_guard<std::mutex> lock_guard{mu_};

  if (has_ended_ == true)
  {
    return;
  }
  has_ended_ = true;

  if (recordable_ == nullptr)
  {
    return;
  }

  auto end_steady_time = NowOr(options.end_steady_time);
  recordable_->SetDuration(std::chrono::steady_clock::time_point(end_steady_time) -
                           std::chrono::steady_clock::time_point(start_steady_time));

  tracer_->GetProcessor().OnEnd(std::move(recordable_));
  recordable_.reset();
}

bool Span::IsRecording() const noexcept
{
  std::lock_guard<std::mutex> lock_guard{mu_};
  return recordable_ != nullptr;
}
}  // namespace trace
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
