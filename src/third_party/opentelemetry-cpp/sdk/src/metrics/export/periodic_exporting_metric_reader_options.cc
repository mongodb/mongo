// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <chrono>

#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/sdk/common/env_variables.h"
#include "opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_options.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{

std::chrono::milliseconds GetEnvDuration(nostd::string_view env_var_name,
                                         std::chrono::milliseconds default_value)
{
  std::chrono::system_clock::duration duration;
  if (common::GetDurationEnvironmentVariable(env_var_name.data(), duration))
  {
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration);
  }
  return default_value;
}

PeriodicExportingMetricReaderOptions::PeriodicExportingMetricReaderOptions()
    : export_interval_millis(GetEnvDuration("OTEL_METRIC_EXPORT_INTERVAL", kExportIntervalMillis)),
      export_timeout_millis(GetEnvDuration("OTEL_METRIC_EXPORT_TIMEOUT", kExportTimeOutMillis))
{}

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
