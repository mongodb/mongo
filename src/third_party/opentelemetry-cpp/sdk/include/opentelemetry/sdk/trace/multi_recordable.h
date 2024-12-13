// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <map>

#include "opentelemetry/common/timestamp.h"
#include "opentelemetry/sdk/trace/processor.h"
#include "opentelemetry/sdk/trace/recordable.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace trace
{
namespace
{
std::size_t MakeKey(const SpanProcessor &processor)
{
  return reinterpret_cast<std::size_t>(&processor);
}

}  // namespace

class MultiRecordable : public Recordable
{
public:
  void AddRecordable(const SpanProcessor &processor,
                     std::unique_ptr<Recordable> recordable) noexcept
  {
    recordables_[MakeKey(processor)] = std::move(recordable);
  }

  const std::unique_ptr<Recordable> &GetRecordable(const SpanProcessor &processor) const noexcept
  {
    // TODO - return nullptr ref on failed lookup?
    auto i = recordables_.find(MakeKey(processor));
    if (i != recordables_.end())
    {
      return i->second;
    }
    static std::unique_ptr<Recordable> empty(nullptr);
    return empty;
  }

  std::unique_ptr<Recordable> ReleaseRecordable(const SpanProcessor &processor) noexcept
  {
    auto i = recordables_.find(MakeKey(processor));
    if (i != recordables_.end())
    {
      std::unique_ptr<Recordable> result(i->second.release());
      recordables_.erase(MakeKey(processor));
      return result;
    }
    return std::unique_ptr<Recordable>(nullptr);
  }

  void SetIdentity(const opentelemetry::trace::SpanContext &span_context,
                   opentelemetry::trace::SpanId parent_span_id) noexcept override
  {
    for (auto &recordable : recordables_)
    {
      recordable.second->SetIdentity(span_context, parent_span_id);
    }
  }

  void SetAttribute(nostd::string_view key,
                    const opentelemetry::common::AttributeValue &value) noexcept override
  {
    for (auto &recordable : recordables_)
    {
      recordable.second->SetAttribute(key, value);
    }
  }

  void AddEvent(nostd::string_view name,
                opentelemetry::common::SystemTimestamp timestamp,
                const opentelemetry::common::KeyValueIterable &attributes) noexcept override
  {

    for (auto &recordable : recordables_)
    {
      recordable.second->AddEvent(name, timestamp, attributes);
    }
  }

  void AddLink(const opentelemetry::trace::SpanContext &span_context,
               const opentelemetry::common::KeyValueIterable &attributes) noexcept override
  {
    for (auto &recordable : recordables_)
    {
      recordable.second->AddLink(span_context, attributes);
    }
  }

  void SetStatus(opentelemetry::trace::StatusCode code,
                 nostd::string_view description) noexcept override
  {
    for (auto &recordable : recordables_)
    {
      recordable.second->SetStatus(code, description);
    }
  }

  void SetName(nostd::string_view name) noexcept override
  {
    for (auto &recordable : recordables_)
    {
      recordable.second->SetName(name);
    }
  }

  void SetTraceFlags(opentelemetry::trace::TraceFlags flags) noexcept override
  {
    for (auto &recordable : recordables_)
    {
      recordable.second->SetTraceFlags(flags);
    }
  }

  void SetSpanKind(opentelemetry::trace::SpanKind span_kind) noexcept override
  {
    for (auto &recordable : recordables_)
    {
      recordable.second->SetSpanKind(span_kind);
    }
  }

  void SetResource(const opentelemetry::sdk::resource::Resource &resource) noexcept override
  {
    for (auto &recordable : recordables_)
    {
      recordable.second->SetResource(resource);
    }
  }

  void SetStartTime(opentelemetry::common::SystemTimestamp start_time) noexcept override
  {
    for (auto &recordable : recordables_)
    {
      recordable.second->SetStartTime(start_time);
    }
  }

  void SetDuration(std::chrono::nanoseconds duration) noexcept override
  {
    for (auto &recordable : recordables_)
    {
      recordable.second->SetDuration(duration);
    }
  }

  void SetInstrumentationScope(const InstrumentationScope &instrumentation_scope) noexcept override
  {
    for (auto &recordable : recordables_)
    {
      recordable.second->SetInstrumentationScope(instrumentation_scope);
    }
  }

private:
  std::map<std::size_t, std::unique_ptr<Recordable>> recordables_;
};
}  // namespace trace
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
