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
   * Create a MeterContext with valid defaults.
   * @return A unique pointer to the created MeterContext object.
   */
  static std::unique_ptr<MeterContext> Create();

  /**
   * Create a MeterContext with specified views.
   * @param views ViewRegistry containing OpenTelemetry views registered with this meter context.
   */
  static std::unique_ptr<MeterContext> Create(std::unique_ptr<ViewRegistry> views);

  /**
   * Create a MeterContext with specified views and resource.
   * @param views ViewRegistry containing OpenTelemetry views registered with this meter context.
   * @param resource The OpenTelemetry resource associated with this meter context.
   * @return A unique pointer to the created MeterContext object.
   */
  static std::unique_ptr<MeterContext> Create(
      std::unique_ptr<ViewRegistry> views,
      const opentelemetry::sdk::resource::Resource &resource);

  /**
   * Create a MeterContext with specified views, resource and meter scope configurator.
   * @param views ViewRegistry containing OpenTelemetry views registered with this meter context.
   * @param resource The OpenTelemetry resource associated with this meter context.
   * @param meter_configurator A scope configurator defining the behavior of a meter associated with
   * this meter context.
   * @return A unique pointer to the created MeterContext object.
   */
  static std::unique_ptr<MeterContext> Create(
      std::unique_ptr<ViewRegistry> views,
      const opentelemetry::sdk::resource::Resource &resource,
      std::unique_ptr<instrumentationscope::ScopeConfigurator<MeterConfig>> meter_configurator);
};

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
