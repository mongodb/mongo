// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>

#include "opentelemetry/version.h"

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

struct OPENTELEMETRY_EXPORT PeriodicExportingMetricReaderOptions
{
  /* The time interval between two consecutive exports. */
  std::chrono::milliseconds export_interval_millis;

  /*  how long the export can run before it is cancelled. */
  std::chrono::milliseconds export_timeout_millis;

  PeriodicExportingMetricReaderOptions();
};

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
