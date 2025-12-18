// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>

#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{

namespace trace
{

/**
 * Struct to hold batch SpanProcessor options.
 */
struct OPENTELEMETRY_EXPORT BatchSpanProcessorOptions
{
  BatchSpanProcessorOptions();
  /**
   * The maximum buffer/queue size. After the size is reached, spans are
   * dropped.
   */
  size_t max_queue_size;

  /* The time interval between two consecutive exports. */
  std::chrono::milliseconds schedule_delay_millis;

  /**
   * The maximum time allowed to to export data
   * It is not currently used by the SDK and the parameter is ignored
   * TODO: Implement the parameter in BatchSpanProcessor
   */
  std::chrono::milliseconds export_timeout;

  /**
   * The maximum batch size of every export. It must be smaller or
   * equal to max_queue_size.
   */
  size_t max_export_batch_size;
};

}  // namespace trace
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
