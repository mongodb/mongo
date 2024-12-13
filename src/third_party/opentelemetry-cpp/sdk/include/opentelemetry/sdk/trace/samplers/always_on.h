// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/sdk/trace/sampler.h"
#include "opentelemetry/trace/span_context.h"
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
class SpanContextKeyValueIterable;
}  // namespace trace

namespace sdk
{
namespace trace
{
/**
 * The always on sampler is a default sampler which always return Decision::RECORD_AND_SAMPLE
 */
class AlwaysOnSampler : public Sampler
{
public:
  /**
   * @return Always return Decision RECORD_AND_SAMPLE
   */
  inline SamplingResult ShouldSample(
      const opentelemetry::trace::SpanContext &parent_context,
      opentelemetry::trace::TraceId /*trace_id*/,
      nostd::string_view /*name*/,
      opentelemetry::trace::SpanKind /*span_kind*/,
      const opentelemetry::common::KeyValueIterable & /*attributes*/,
      const opentelemetry::trace::SpanContextKeyValueIterable & /*links*/) noexcept override
  {
    if (!parent_context.IsValid())
    {
      return {Decision::RECORD_AND_SAMPLE, nullptr, opentelemetry::trace::TraceState::GetDefault()};
    }
    else
    {
      return {Decision::RECORD_AND_SAMPLE, nullptr, parent_context.trace_state()};
    }
  }

  /**
   * @return Description MUST be AlwaysOnSampler
   */
  inline nostd::string_view GetDescription() const noexcept override { return "AlwaysOnSampler"; }
};
}  // namespace trace
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
