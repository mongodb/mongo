// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <mutex>

#include "opentelemetry/nostd/shared_ptr.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{

namespace metrics
{

/**
 * Changes the singleton global MeterProvider.
 */
class Provider
{
public:
  /**
   * Changes the singleton MeterProvider.
   */
  static void SetMeterProvider(
      const nostd::shared_ptr<opentelemetry::metrics::MeterProvider> &mp) noexcept;
};

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
