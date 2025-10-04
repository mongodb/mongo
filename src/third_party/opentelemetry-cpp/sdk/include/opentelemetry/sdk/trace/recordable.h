// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>

#include "opentelemetry/common/attribute_value.h"
#include "opentelemetry/common/timestamp.h"
#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/sdk/common/empty_attributes.h"
#include "opentelemetry/sdk/instrumentationscope/instrumentation_scope.h"
#include "opentelemetry/trace/span_context.h"
#include "opentelemetry/trace/span_id.h"
#include "opentelemetry/trace/span_metadata.h"
#include "opentelemetry/version.h"

// TODO: Create generic short pattern for opentelemetry::common and opentelemetry::trace

OPENTELEMETRY_BEGIN_NAMESPACE
namespace common
{
class KeyValueIterable;
}  // namespace common

namespace sdk
{
namespace resource
{
class Resource;
}  // namespace resource

namespace trace
{

using namespace opentelemetry::sdk::instrumentationscope;
class SpanData;

/**
 * Maintains a representation of a span in a format that can be processed by a recorder.
 *
 * This class is thread-compatible.
 */
class Recordable
{
public:
  virtual ~Recordable() = default;

  /**
   * Set the span context and parent span id
   * @param span_context the span context to set
   * @param parent_span_id the parent span id to set
   */
  virtual void SetIdentity(const opentelemetry::trace::SpanContext &span_context,
                           opentelemetry::trace::SpanId parent_span_id) noexcept = 0;

  /**
   * Set an attribute of a span.
   * @param name the name of the attribute
   * @param value the attribute value
   */
  virtual void SetAttribute(nostd::string_view key,
                            const opentelemetry::common::AttributeValue &value) noexcept = 0;

  /**
   * Add an event to a span.
   * @param name the name of the event
   * @param timestamp the timestamp of the event
   * @param attributes the attributes associated with the event
   */
  virtual void AddEvent(nostd::string_view name,
                        opentelemetry::common::SystemTimestamp timestamp,
                        const opentelemetry::common::KeyValueIterable &attributes) noexcept = 0;

  /**
   * Add an event to a span with default timestamp and attributes.
   * @param name the name of the event
   */
  void AddEvent(nostd::string_view name)
  {
    AddEvent(name, opentelemetry::common::SystemTimestamp(std::chrono::system_clock::now()),
             opentelemetry::sdk::GetEmptyAttributes());
  }

  /**
   * Add an event to a span with default (empty) attributes.
   * @param name the name of the event
   * @param timestamp the timestamp of the event
   */
  void AddEvent(nostd::string_view name, opentelemetry::common::SystemTimestamp timestamp)
  {
    AddEvent(name, timestamp, opentelemetry::sdk::GetEmptyAttributes());
  }

  /**
   * Add an event to a span.
   * @param name the name of the event
   * @param attributes the attributes associated with the event
   */
  void AddEvent(nostd::string_view name,
                const opentelemetry::common::KeyValueIterable &attributes) noexcept
  {
    AddEvent(name, opentelemetry::common::SystemTimestamp(std::chrono::system_clock::now()),
             attributes);
  }

  /**
   * Add a link to a span.
   * @param span_context the span context of the linked span
   * @param attributes the attributes associated with the link
   */
  virtual void AddLink(const opentelemetry::trace::SpanContext &span_context,
                       const opentelemetry::common::KeyValueIterable &attributes) noexcept = 0;

  /**
   * Add a link to a span with default (empty) attributes.
   * @param span_context the span context of the linked span
   */
  void AddLink(opentelemetry::trace::SpanContext span_context)
  {
    AddLink(span_context, opentelemetry::sdk::GetEmptyAttributes());
  }

  /**
   * Set the status of the span.
   * @param code the status code
   * @param description a description of the status
   */
  virtual void SetStatus(opentelemetry::trace::StatusCode code,
                         nostd::string_view description) noexcept = 0;

  /**
   * Set the name of the span.
   * @param name the name to set
   */
  virtual void SetName(nostd::string_view name) noexcept = 0;

  /**
   * Set the trace flags of the span.
   * @param flags the flags to set
   */
  virtual void SetTraceFlags(opentelemetry::trace::TraceFlags /* flags */) noexcept {}

  /**
   * Set the spankind of the span.
   * @param span_kind the spankind to set
   */
  virtual void SetSpanKind(opentelemetry::trace::SpanKind span_kind) noexcept = 0;

  /**
   * Set Resource of the span
   * @param Resource the resource to set
   */
  virtual void SetResource(const opentelemetry::sdk::resource::Resource &resource) noexcept = 0;

  /**
   * Set the start time of the span.
   * @param start_time the start time to set
   */
  virtual void SetStartTime(opentelemetry::common::SystemTimestamp start_time) noexcept = 0;

  /**
   * Set the duration of the span.
   * @param duration the duration to set
   */
  virtual void SetDuration(std::chrono::nanoseconds duration) noexcept = 0;

  /**
   * Get the SpanData object for this Recordable.
   *
   * @return SpanData*
   */

  virtual explicit operator SpanData *() const { return nullptr; }

  /**
   * Set the instrumentation scope of the span.
   * @param instrumentation_scope the instrumentation scope to set
   */
  virtual void SetInstrumentationScope(
      const InstrumentationScope &instrumentation_scope) noexcept = 0;

  OPENTELEMETRY_DEPRECATED_MESSAGE("Please use SetInstrumentationScope instead")
  void SetInstrumentationLibrary(const InstrumentationScope &instrumentation_scope) noexcept
  {
    SetInstrumentationScope(instrumentation_scope);
  }
};
}  // namespace trace
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
