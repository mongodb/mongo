// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include "opentelemetry/sdk/trace/exporter.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace trace
{

OPENTELEMETRY_EXPORT SpanExporter::SpanExporter() {}

OPENTELEMETRY_EXPORT SpanExporter::~SpanExporter() {}

OPENTELEMETRY_EXPORT bool SpanExporter::ForceFlush(std::chrono::microseconds /*timeout*/) noexcept
{
  return true;
}

}  // namespace trace
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
