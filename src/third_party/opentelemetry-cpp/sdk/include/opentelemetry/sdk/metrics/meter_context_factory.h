// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>

#include "opentelemetry/sdk/metrics/meter_context.h"
#include "opentelemetry/sdk/metrics/view/view_registry.h"
#include "opentelemetry/sdk/resource/resource.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{

/**
 * Factory class for MeterContext.
 */
class OPENTELEMETRY_EXPORT MeterContextFactory
{
public:
  /**
   * Create a MeterContext.
   */
  static std::unique_ptr<MeterContext> Create();

  static std::unique_ptr<MeterContext> Create(std::unique_ptr<ViewRegistry> views);

  static std::unique_ptr<MeterContext> Create(
      std::unique_ptr<ViewRegistry> views,
      const opentelemetry::sdk::resource::Resource &resource);
};

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
