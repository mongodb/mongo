// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <stddef.h>
#include <chrono>
#include <cstdint>

#include "opentelemetry/sdk/common/env_variables.h"
#include "opentelemetry/sdk/trace/batch_span_processor_options.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace trace
{

constexpr const char *kMaxQueueSizeEnv       = "OTEL_BSP_MAX_QUEUE_SIZE";
constexpr const char *kScheduleDelayEnv      = "OTEL_BSP_SCHEDULE_DELAY";
constexpr const char *kExportTimeoutEnv      = "OTEL_BSP_EXPORT_TIMEOUT";
constexpr const char *kMaxExportBatchSizeEnv = "OTEL_BSP_MAX_EXPORT_BATCH_SIZE";

const size_t kDefaultMaxQueueSize                           = 2084;
const std::chrono::milliseconds kDefaultScheduleDelayMillis = std::chrono::milliseconds(5000);
const std::chrono::milliseconds kDefaultExportTimeout       = std::chrono::milliseconds(3000);
const size_t kDefaultMaxExportBatchSize                     = 512;

inline size_t GetMaxQueueSizeFromEnv()
{
  std::uint32_t value{};
  if (!opentelemetry::sdk::common::GetUintEnvironmentVariable(kMaxQueueSizeEnv, value))
  {
    return kDefaultMaxQueueSize;
  }
  return static_cast<size_t>(value);
}

inline std::chrono::milliseconds GetDurationFromEnv(
    const char *env_var,
    const std::chrono::milliseconds &default_duration)
{
  std::chrono::system_clock::duration duration{0};
  if (!opentelemetry::sdk::common::GetDurationEnvironmentVariable(env_var, duration))
  {
    return default_duration;
  }
  return std::chrono::duration_cast<std::chrono::milliseconds>(duration);
}

inline size_t GetMaxExportBatchSizeFromEnv()
{
  std::uint32_t value{};
  if (!opentelemetry::sdk::common::GetUintEnvironmentVariable(kMaxExportBatchSizeEnv, value))
  {
    return kDefaultMaxExportBatchSize;
  }
  return static_cast<size_t>(value);
}

BatchSpanProcessorOptions::BatchSpanProcessorOptions()
    : max_queue_size(GetMaxQueueSizeFromEnv()),
      schedule_delay_millis(GetDurationFromEnv(kScheduleDelayEnv, kDefaultScheduleDelayMillis)),
      export_timeout(GetDurationFromEnv(kExportTimeoutEnv, kDefaultExportTimeout)),
      max_export_batch_size(GetMaxExportBatchSizeFromEnv())
{}

}  // namespace trace
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
