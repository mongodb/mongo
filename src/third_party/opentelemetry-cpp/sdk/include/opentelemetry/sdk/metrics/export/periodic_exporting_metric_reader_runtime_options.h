// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>

#include "opentelemetry/sdk/common/thread_instrumentation.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{

/**
 * Struct to hold PeriodicExortingMetricReader runtime options.
 */
struct PeriodicExportingMetricReaderRuntimeOptions
{
  std::shared_ptr<sdk::common::ThreadInstrumentation> periodic_thread_instrumentation =
      std::shared_ptr<sdk::common::ThreadInstrumentation>(nullptr);
  std::shared_ptr<sdk::common::ThreadInstrumentation> collect_thread_instrumentation =
      std::shared_ptr<sdk::common::ThreadInstrumentation>(nullptr);
};

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
