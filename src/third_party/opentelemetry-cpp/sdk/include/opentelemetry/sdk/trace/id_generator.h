// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "opentelemetry/trace/span_id.h"
#include "opentelemetry/trace/trace_id.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace trace
{

/** IdGenerator provides an interface for generating Trace Id and Span Id */
class IdGenerator
{
public:
  IdGenerator(bool is_random) : is_random_(is_random) {}

  virtual ~IdGenerator() = default;

  /** Returns a SpanId represented by opaque 128-bit trace identifier */
  virtual opentelemetry::trace::SpanId GenerateSpanId() noexcept = 0;

  /** Returns a TraceId represented by opaque 64-bit trace identifier */
  virtual opentelemetry::trace::TraceId GenerateTraceId() noexcept = 0;

  bool IsRandom() const { return is_random_; }

private:
  /** True if GenerateTraceId() is random,
   * per https://www.w3.org/TR/trace-context-2/#random-trace-id-flag
   */
  bool is_random_;
};
}  // namespace trace

}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
