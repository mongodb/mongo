// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <map>
#include <memory>
#include <string>

#include "opentelemetry/common/attribute_value.h"
#include "opentelemetry/nostd/shared_ptr.h"
#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/trace/span_metadata.h"
#include "opentelemetry/trace/trace_id.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace common
{
class KeyValueIterable;
}  // namespace common

namespace trace
{
class SpanContext;
class SpanContextKeyValueIterable;
class TraceState;
}  // namespace trace

namespace sdk
{
namespace trace
{
/**
 * A sampling Decision for a Span to be created.
 */
enum class Decision
{
  // IsRecording() == false, span will not be recorded and all events and attributes will be
  // dropped.
  DROP,
  // IsRecording() == true, but Sampled flag MUST NOT be set.
  RECORD_ONLY,
  // IsRecording() == true AND Sampled flag` MUST be set.
  RECORD_AND_SAMPLE
};

/**
 * The output of ShouldSample.
 * It contains a sampling Decision and a set of Span Attributes.
 */
struct SamplingResult
{
  Decision decision;
  // A set of span Attributes that will also be added to the Span. Can be nullptr.
  std::unique_ptr<const std::map<std::string, opentelemetry::common::AttributeValue>> attributes;
  //  The tracestate used by the span.
  nostd::shared_ptr<opentelemetry::trace::TraceState> trace_state;

  inline bool IsRecording()
  {
    return decision == Decision::RECORD_ONLY || decision == Decision::RECORD_AND_SAMPLE;
  }
  inline bool IsSampled() { return decision == Decision::RECORD_AND_SAMPLE; }
};

/**
 * The Sampler interface allows users to create custom samplers which will return a
 * SamplingResult based on information that is typically available just before the Span was created.
 */
class Sampler
{
public:
  virtual ~Sampler() = default;
  /**
   * Called during Span creation to make a sampling decision.
   *
   * @param parent_context a const reference to the SpanContext of a parent Span.
   *        An invalid SpanContext if this is a root span.
   * @param trace_id the TraceId for the new Span. This will be identical to that in
   *        the parentContext, unless this is a root span.
   * @param name the name of the new Span.
   * @param spanKind the opentelemetry::trace::SpanKind of the Span.
   * @param attributes list of AttributeValue with their keys.
   * @param links Collection of links that will be associated with the Span to be created.
   * @return sampling result whether span should be sampled or not.
   * @since 0.1.0
   */

  virtual SamplingResult ShouldSample(
      const opentelemetry::trace::SpanContext &parent_context,
      opentelemetry::trace::TraceId trace_id,
      nostd::string_view name,
      opentelemetry::trace::SpanKind span_kind,
      const opentelemetry::common::KeyValueIterable &attributes,
      const opentelemetry::trace::SpanContextKeyValueIterable &links) noexcept = 0;

  /**
   * Returns the sampler name or short description with the configuration.
   * This may be displayed on debug pages or in the logs.
   *
   * @return the description of this Sampler.
   */
  virtual nostd::string_view GetDescription() const noexcept = 0;
};
}  // namespace trace
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
