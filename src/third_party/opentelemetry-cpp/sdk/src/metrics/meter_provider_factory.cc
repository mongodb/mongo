// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <memory>
#include <utility>

#include <vector>
#include "opentelemetry/sdk/instrumentationscope/scope_configurator.h"
#include "opentelemetry/sdk/metrics/meter_config.h"
#include "opentelemetry/sdk/metrics/meter_context.h"
#include "opentelemetry/sdk/metrics/meter_provider.h"
#include "opentelemetry/sdk/metrics/meter_provider_factory.h"
#include "opentelemetry/sdk/metrics/view/view_registry.h"
#include "opentelemetry/sdk/metrics/view/view_registry_factory.h"
#include "opentelemetry/sdk/resource/resource.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{

std::unique_ptr<opentelemetry::sdk::metrics::MeterProvider> MeterProviderFactory::Create()
{
  auto views = ViewRegistryFactory::Create();
  return Create(std::move(views));
}

std::unique_ptr<opentelemetry::sdk::metrics::MeterProvider> MeterProviderFactory::Create(
    std::unique_ptr<ViewRegistry> views)
{
  auto resource = opentelemetry::sdk::resource::Resource::Create({});
  return Create(std::move(views), resource);
}

std::unique_ptr<opentelemetry::sdk::metrics::MeterProvider> MeterProviderFactory::Create(
    std::unique_ptr<ViewRegistry> views,
    const opentelemetry::sdk::resource::Resource &resource)
{
  auto meter_configurator = std::make_unique<instrumentationscope::ScopeConfigurator<MeterConfig>>(
      instrumentationscope::ScopeConfigurator<MeterConfig>::Builder(MeterConfig::Default())
          .Build());
  return Create(std::move(views), resource, std::move(meter_configurator));
}

std::unique_ptr<opentelemetry::sdk::metrics::MeterProvider> MeterProviderFactory::Create(
    std::unique_ptr<ViewRegistry> views,
    const opentelemetry::sdk::resource::Resource &resource,
    std::unique_ptr<instrumentationscope::ScopeConfigurator<MeterConfig>> meter_configurator)
{
  std::unique_ptr<opentelemetry::sdk::metrics::MeterProvider> provider(
      new opentelemetry::sdk::metrics::MeterProvider(std::move(views), resource,
                                                     std::move(meter_configurator)));
  return provider;
}

std::unique_ptr<opentelemetry::sdk::metrics::MeterProvider> MeterProviderFactory::Create(
    std::unique_ptr<sdk::metrics::MeterContext> context)
{
  std::unique_ptr<opentelemetry::sdk::metrics::MeterProvider> provider(
      new opentelemetry::sdk::metrics::MeterProvider(std::move(context)));
  return provider;
}

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
