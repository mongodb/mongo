// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "opentelemetry/version.h"

#include <chrono>

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{

constexpr std::chrono::milliseconds kExportIntervalMillis = std::chrono::milliseconds(60000);
constexpr std::chrono::milliseconds kExportTimeOutMillis  = std::chrono::milliseconds(30000);

/**
 * Struct to hold PeriodicExortingMetricReader options.
 */

struct PeriodicExportingMetricReaderOptions
{
  /* The time interval between two consecutive exports. */
  std::chrono::milliseconds export_interval_millis =
      std::chrono::milliseconds(kExportIntervalMillis);

  /*  how long the export can run before it is cancelled. */
  std::chrono::milliseconds export_timeout_millis = std::chrono::milliseconds(kExportTimeOutMillis);
};

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
