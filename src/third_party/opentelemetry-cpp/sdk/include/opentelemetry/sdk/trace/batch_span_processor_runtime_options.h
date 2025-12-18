// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>

#include "opentelemetry/sdk/common/thread_instrumentation.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{

namespace trace
{

/**
 * Struct to hold batch SpanProcessor runtime options.
 */
struct BatchSpanProcessorRuntimeOptions
{
  std::shared_ptr<sdk::common::ThreadInstrumentation> thread_instrumentation =
      std::shared_ptr<sdk::common::ThreadInstrumentation>(nullptr);
};

}  // namespace trace
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
