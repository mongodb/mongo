// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>

#include "opentelemetry/nostd/span.h"
#include "opentelemetry/sdk/trace/recordable.h"
#include "opentelemetry/version.h"

namespace opentelemetry
{
namespace proto
{
namespace collector
{

namespace trace
{
namespace v1
{
class ExportTraceServiceRequest;
}
}  // namespace trace

}  // namespace collector
}  // namespace proto
}  // namespace opentelemetry

OPENTELEMETRY_BEGIN_NAMESPACE
namespace exporter
{
namespace otlp
{
/**
 * The OtlpRecordableUtils contains utility functions for OTLP recordable
 */
class OtlpRecordableUtils
{
public:
  static void PopulateRequest(
      const nostd::span<std::unique_ptr<opentelemetry::sdk::trace::Recordable>> &spans,
      proto::collector::trace::v1::ExportTraceServiceRequest *request) noexcept;
};
}  // namespace otlp
}  // namespace exporter
OPENTELEMETRY_END_NAMESPACE
