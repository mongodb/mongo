// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <stdint.h>
#include <string>

#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/sdk/trace/sampler.h"
#include "opentelemetry/trace/span_metadata.h"
#include "opentelemetry/trace/trace_id.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace trace
{
/**
 * The TraceIdRatioBased sampler computes and returns a decision based on the
 * provided trace_id and the configured ratio.
 */
class TraceIdRatioBasedSampler : public Sampler
{
public:
  /**
   * @param ratio a required value, 1.0 >= ratio >= 0.0. If the given trace_id
   * falls into a given ratio of all possible trace_id values, ShouldSample will
   * return RECORD_AND_SAMPLE.
   * @throws invalid_argument if ratio is out of bounds [0.0, 1.0]
   */
  explicit TraceIdRatioBasedSampler(double ratio);

  /**
   * @return Returns either RECORD_AND_SAMPLE or DROP based on current
   * sampler configuration and provided trace_id and ratio. trace_id
   * is used as a pseudorandom value in conjunction with the predefined
   * ratio to determine whether this trace should be sampled
   */
  SamplingResult ShouldSample(
      const opentelemetry::trace::SpanContext & /*parent_context*/,
      opentelemetry::trace::TraceId trace_id,
      nostd::string_view /*name*/,
      opentelemetry::trace::SpanKind /*span_kind*/,
      const opentelemetry::common::KeyValueIterable & /*attributes*/,
      const opentelemetry::trace::SpanContextKeyValueIterable & /*links*/) noexcept override;

  /**
   * @return Description MUST be TraceIdRatioBasedSampler{0.000100}
   */
  nostd::string_view GetDescription() const noexcept override;

private:
  std::string description_;
  const uint64_t threshold_;
};
}  // namespace trace
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
