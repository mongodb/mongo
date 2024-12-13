// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>

#include "opentelemetry/sdk/trace/exporter.h"
#include "opentelemetry/sdk/trace/processor.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace trace
{

/**
 * Factory class for SimpleSpanProcessor.
 */
class OPENTELEMETRY_EXPORT SimpleSpanProcessorFactory
{
public:
  /**
   * Create a SimpleSpanProcessor.
   */
  static std::unique_ptr<SpanProcessor> Create(std::unique_ptr<SpanExporter> &&exporter);
};

}  // namespace trace
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
