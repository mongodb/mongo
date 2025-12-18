// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "opentelemetry/nostd/shared_ptr.h"
#include "opentelemetry/trace/tracer_provider.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace trace
{

/**
 * Changes the singleton global TracerProvider.
 */
class OPENTELEMETRY_EXPORT Provider
{
public:
  /**
   * Changes the singleton TracerProvider.
   */
  static void SetTracerProvider(
      const nostd::shared_ptr<opentelemetry::trace::TracerProvider> &tp) noexcept;
};

}  // namespace trace
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
