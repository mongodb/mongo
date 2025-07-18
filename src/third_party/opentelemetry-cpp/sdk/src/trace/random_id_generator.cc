// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <stdint.h>

#include "opentelemetry/nostd/span.h"
#include "opentelemetry/sdk/trace/random_id_generator.h"
#include "opentelemetry/trace/span_id.h"
#include "opentelemetry/trace/trace_id.h"
#include "opentelemetry/version.h"
#include "src/common/random.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace trace
{
namespace trace_api = opentelemetry::trace;

trace_api::SpanId RandomIdGenerator::GenerateSpanId() noexcept
{
  uint8_t span_id_buf[trace_api::SpanId::kSize];
  sdk::common::Random::GenerateRandomBuffer(span_id_buf);
  return trace_api::SpanId(span_id_buf);
}

trace_api::TraceId RandomIdGenerator::GenerateTraceId() noexcept
{
  uint8_t trace_id_buf[trace_api::TraceId::kSize];
  sdk::common::Random::GenerateRandomBuffer(trace_id_buf);
  return trace_api::TraceId(trace_id_buf);
}
}  // namespace trace
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
