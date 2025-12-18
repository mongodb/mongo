// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "opentelemetry/common/attribute_value.h"
#include "opentelemetry/common/key_value_iterable.h"
#include "opentelemetry/common/key_value_iterable_view.h"
#include "opentelemetry/common/timestamp.h"
#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/sdk/common/attribute_utils.h"
#include "opentelemetry/sdk/instrumentationscope/instrumentation_scope.h"
#include "opentelemetry/sdk/resource/resource.h"
#include "opentelemetry/sdk/trace/recordable.h"
#include "opentelemetry/trace/span_context.h"
#include "opentelemetry/trace/span_id.h"
#include "opentelemetry/trace/span_metadata.h"
#include "opentelemetry/trace/trace_flags.h"
#include "opentelemetry/trace/trace_id.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace trace
{
/**
 * Class for storing events in SpanData.
 */
class SpanDataEvent
{
public:
  SpanDataEvent(std::string name,
                opentelemetry::common::SystemTimestamp timestamp,
                const opentelemetry::common::KeyValueIterable &attributes)
      : name_(name), timestamp_(timestamp), attribute_map_(attributes)
  {}

  /**
   * Get the name for this event
   * @return the name for this event
   */
  std::string GetName() const noexcept { return name_; }

  /**
   * Get the timestamp for this event
   * @return the timestamp for this event
   */
  opentelemetry::common::SystemTimestamp GetTimestamp() const noexcept { return timestamp_; }

  /**
   * Get the attributes for this event
   * @return the attributes for this event
   */
  const std::unordered_map<std::string, opentelemetry::sdk::common::OwnedAttributeValue> &
  GetAttributes() const noexcept
  {
    return attribute_map_.GetAttributes();
  }

private:
  std::string name_;
  opentelemetry::common::SystemTimestamp timestamp_;
  opentelemetry::sdk::common::AttributeMap attribute_map_;
};

/**
 * Class for storing links in SpanData.
 */
class SpanDataLink
{
public:
  SpanDataLink(opentelemetry::trace::SpanContext span_context,
               const opentelemetry::common::KeyValueIterable &attributes)
      : span_context_(span_context), attribute_map_(attributes)
  {}

  /**
   * Get the attributes for this link
   * @return the attributes for this link
   */
  const std::unordered_map<std::string, opentelemetry::sdk::common::OwnedAttributeValue> &
  GetAttributes() const noexcept
  {
    return attribute_map_.GetAttributes();
  }

  /**
   * Get the span context for this link
   * @return the span context for this link
   */
  const opentelemetry::trace::SpanContext &GetSpanContext() const noexcept { return span_context_; }

private:
  opentelemetry::trace::SpanContext span_context_;
  opentelemetry::sdk::common::AttributeMap attribute_map_;
};

/**
 * SpanData is a representation of all data collected by a span.
 */
class SpanData final : public Recordable
{
public:
  SpanData() : resource_{nullptr}, instrumentation_scope_{nullptr} {}
  /**
   * Get the trace id for this span
   * @return the trace id for this span
   */
  opentelemetry::trace::TraceId GetTraceId() const noexcept { return span_context_.trace_id(); }

  /**
   * Get the span id for this span
   * @return the span id for this span
   */
  opentelemetry::trace::SpanId GetSpanId() const noexcept { return span_context_.span_id(); }

  /**
   * Get the span context for this span
   * @return the span context for this span
   */
  const opentelemetry::trace::SpanContext &GetSpanContext() const noexcept { return span_context_; }

  /**
   * Get the parent span id for this span
   * @return the span id for this span's parent
   */
  opentelemetry::trace::SpanId GetParentSpanId() const noexcept { return parent_span_id_; }

  /**
   * Get the name for this span
   * @return the name for this span
   */
  opentelemetry::nostd::string_view GetName() const noexcept { return name_; }

  /**
   * Get the trace flags for this span
   * @return the trace flags for this span
   */
  opentelemetry::trace::TraceFlags GetFlags() const noexcept { return flags_; }

  /**
   * Get the kind of this span
   * @return the kind of this span
   */
  opentelemetry::trace::SpanKind GetSpanKind() const noexcept { return span_kind_; }

  /**
   * Get the status for this span
   * @return the status for this span
   */
  opentelemetry::trace::StatusCode GetStatus() const noexcept { return status_code_; }

  /**
   * Get the status description for this span
   * @return the description of the the status of this span
   */
  opentelemetry::nostd::string_view GetDescription() const noexcept { return status_desc_; }

  /**
   * Get the attributes associated with the resource
   * @returns the attributes associated with the resource configured for TracerProvider
   */

  const opentelemetry::sdk::resource::Resource &GetResource() const noexcept
  {
    if (resource_ == nullptr)
    {
      // this shouldn't happen as TraceProvider provides default resources
      static opentelemetry::sdk::resource::Resource resource =
          opentelemetry::sdk::resource::Resource::GetEmpty();
      return resource;
    }
    return *resource_;
  }

  /**
   * Get the attributes associated with the resource
   * @returns the attributes associated with the resource configured for TracerProvider
   */

  const opentelemetry::sdk::trace::InstrumentationScope &GetInstrumentationScope() const noexcept
  {
    if (instrumentation_scope_ == nullptr)
    {
      // this shouldn't happen as Tracer ensures there is valid default instrumentation scope.
      static std::unique_ptr<opentelemetry::sdk::instrumentationscope::InstrumentationScope>
          instrumentation_scope =
              opentelemetry::sdk::instrumentationscope::InstrumentationScope::Create(
                  "unknown_service");
      return *instrumentation_scope;
    }
    return *instrumentation_scope_;
  }

  OPENTELEMETRY_DEPRECATED_MESSAGE("Please use GetInstrumentationScope instead")
  const opentelemetry::sdk::trace::InstrumentationScope &GetInstrumentationLibrary() const noexcept
  {
    return GetInstrumentationScope();
  }

  /**
   * Get the start time for this span
   * @return the start time for this span
   */
  opentelemetry::common::SystemTimestamp GetStartTime() const noexcept { return start_time_; }

  /**
   * Get the duration for this span
   * @return the duration for this span
   */
  std::chrono::nanoseconds GetDuration() const noexcept { return duration_; }

  /**
   * Get the attributes for this span
   * @return the attributes for this span
   */
  const std::unordered_map<std::string, opentelemetry::sdk::common::OwnedAttributeValue> &
  GetAttributes() const noexcept
  {
    return attribute_map_.GetAttributes();
  }

  /**
   * Get the events associated with this span
   * @return the events associated with this span
   */
  const std::vector<SpanDataEvent> &GetEvents() const noexcept { return events_; }

  /**
   * Get the links associated with this span
   * @return the links associated with this span
   */
  const std::vector<SpanDataLink> &GetLinks() const noexcept { return links_; }

  void SetIdentity(const opentelemetry::trace::SpanContext &span_context,
                   opentelemetry::trace::SpanId parent_span_id) noexcept override
  {
    span_context_   = span_context;
    parent_span_id_ = parent_span_id;
  }

  void SetAttribute(nostd::string_view key,
                    const opentelemetry::common::AttributeValue &value) noexcept override
  {
    attribute_map_.SetAttribute(key, value);
  }

  void AddEvent(nostd::string_view name,
                opentelemetry::common::SystemTimestamp timestamp =
                    opentelemetry::common::SystemTimestamp(std::chrono::system_clock::now()),
                const opentelemetry::common::KeyValueIterable &attributes =
                    opentelemetry::common::KeyValueIterableView<std::map<std::string, int32_t>>(
                        {})) noexcept override
  {
    SpanDataEvent event(std::string(name), timestamp, attributes);
    events_.push_back(event);
  }

  void AddLink(const opentelemetry::trace::SpanContext &span_context,
               const opentelemetry::common::KeyValueIterable &attributes) noexcept override
  {
    SpanDataLink link(span_context, attributes);
    links_.push_back(link);
  }

  void SetStatus(opentelemetry::trace::StatusCode code,
                 nostd::string_view description) noexcept override
  {
    status_code_ = code;
    status_desc_ = std::string(description);
  }

  void SetName(nostd::string_view name) noexcept override
  {
    name_ = std::string(name.data(), name.length());
  }

  void SetTraceFlags(opentelemetry::trace::TraceFlags flags) noexcept override { flags_ = flags; }

  void SetSpanKind(opentelemetry::trace::SpanKind span_kind) noexcept override
  {
    span_kind_ = span_kind;
  }

  void SetResource(const opentelemetry::sdk::resource::Resource &resource) noexcept override
  {
    resource_ = &resource;
  }

  void SetStartTime(opentelemetry::common::SystemTimestamp start_time) noexcept override
  {
    start_time_ = start_time;
  }

  void SetDuration(std::chrono::nanoseconds duration) noexcept override { duration_ = duration; }

  void SetInstrumentationScope(const InstrumentationScope &instrumentation_scope) noexcept override
  {
    instrumentation_scope_ = &instrumentation_scope;
  }

private:
  opentelemetry::trace::SpanContext span_context_{false, false};
  opentelemetry::trace::SpanId parent_span_id_;
  opentelemetry::common::SystemTimestamp start_time_;
  std::chrono::nanoseconds duration_{0};
  std::string name_;
  opentelemetry::trace::StatusCode status_code_{opentelemetry::trace::StatusCode::kUnset};
  std::string status_desc_;
  opentelemetry::sdk::common::AttributeMap attribute_map_;
  std::vector<SpanDataEvent> events_;
  std::vector<SpanDataLink> links_;
  opentelemetry::trace::TraceFlags flags_;
  opentelemetry::trace::SpanKind span_kind_{opentelemetry::trace::SpanKind::kInternal};
  const opentelemetry::sdk::resource::Resource *resource_;
  const InstrumentationScope *instrumentation_scope_;
};
}  // namespace trace
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
