// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <memory>
#include <utility>

#include <vector>
#include "opentelemetry/sdk/instrumentationscope/scope_configurator.h"
#include "opentelemetry/sdk/metrics/meter_config.h"
#include "opentelemetry/sdk/metrics/meter_context.h"
#include "opentelemetry/sdk/metrics/meter_context_factory.h"
#include "opentelemetry/sdk/metrics/view/view_registry.h"
#include "opentelemetry/sdk/metrics/view/view_registry_factory.h"
#include "opentelemetry/sdk/resource/resource.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{

std::unique_ptr<MeterContext> MeterContextFactory::Create()
{
  auto views = ViewRegistryFactory::Create();
  return Create(std::move(views));
}

std::unique_ptr<MeterContext> MeterContextFactory::Create(std::unique_ptr<ViewRegistry> views)
{
  auto resource = opentelemetry::sdk::resource::Resource::Create({});
  return Create(std::move(views), resource);
}

std::unique_ptr<MeterContext> MeterContextFactory::Create(
    std::unique_ptr<ViewRegistry> views,
    const opentelemetry::sdk::resource::Resource &resource)
{
  auto meter_configurator = std::make_unique<instrumentationscope::ScopeConfigurator<MeterConfig>>(
      instrumentationscope::ScopeConfigurator<MeterConfig>::Builder(MeterConfig::Default())
          .Build());
  return Create(std::move(views), resource, std::move(meter_configurator));
}

std::unique_ptr<MeterContext> MeterContextFactory::Create(
    std::unique_ptr<ViewRegistry> views,
    const opentelemetry::sdk::resource::Resource &resource,
    std::unique_ptr<instrumentationscope::ScopeConfigurator<MeterConfig>> meter_configurator)
{
  std::unique_ptr<MeterContext> context(
      new MeterContext(std::move(views), resource, std::move(meter_configurator)));
  return context;
}

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
