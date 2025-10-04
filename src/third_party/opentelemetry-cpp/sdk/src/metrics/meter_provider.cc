// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <mutex>
#include <utility>

#include "opentelemetry/common/key_value_iterable.h"
#include "opentelemetry/metrics/meter.h"
#include "opentelemetry/nostd/shared_ptr.h"
#include "opentelemetry/nostd/span.h"
#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/sdk/common/global_log_handler.h"
#include "opentelemetry/sdk/instrumentationscope/instrumentation_scope.h"
#include "opentelemetry/sdk/metrics/export/metric_producer.h"
#include "opentelemetry/sdk/metrics/meter.h"
#include "opentelemetry/sdk/metrics/meter_context.h"
#include "opentelemetry/sdk/metrics/meter_provider.h"
#include "opentelemetry/sdk/metrics/metric_reader.h"
#include "opentelemetry/sdk/metrics/view/instrument_selector.h"
#include "opentelemetry/sdk/metrics/view/meter_selector.h"
#include "opentelemetry/sdk/metrics/view/view.h"
#include "opentelemetry/sdk/metrics/view/view_registry.h"
#include "opentelemetry/sdk/resource/resource.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{
namespace resource    = opentelemetry::sdk::resource;
namespace metrics_api = opentelemetry::metrics;

MeterProvider::MeterProvider(std::unique_ptr<MeterContext> context) noexcept
    : context_(std::move(context))
{}

MeterProvider::MeterProvider(std::unique_ptr<ViewRegistry> views,
                             const sdk::resource::Resource &resource) noexcept
    : context_(std::make_shared<MeterContext>(std::move(views), resource))
{
  OTEL_INTERNAL_LOG_DEBUG("[MeterProvider] MeterProvider created.");
}

#if OPENTELEMETRY_ABI_VERSION_NO >= 2
nostd::shared_ptr<metrics_api::Meter> MeterProvider::GetMeter(
    nostd::string_view name,
    nostd::string_view version,
    nostd::string_view schema_url,
    const opentelemetry::common::KeyValueIterable *attributes) noexcept
#else
nostd::shared_ptr<metrics_api::Meter> MeterProvider::GetMeter(
    nostd::string_view name,
    nostd::string_view version,
    nostd::string_view schema_url) noexcept
#endif
{
#if OPENTELEMETRY_ABI_VERSION_NO < 2
  const opentelemetry::common::KeyValueIterable *attributes = nullptr;
#endif

  if (name.data() == nullptr || name == "")
  {
    OTEL_INTERNAL_LOG_WARN("[MeterProvider::GetMeter] Library name is empty.");
    name = "";
  }

  const std::lock_guard<std::mutex> guard(lock_);

  for (auto &meter : context_->GetMeters())
  {
    auto meter_lib = meter->GetInstrumentationScope();
    if (meter_lib->equal(name, version, schema_url))
    {
      return nostd::shared_ptr<metrics_api::Meter>{meter};
    }
  }

  instrumentationscope::InstrumentationScopeAttributes attrs_map(attributes);
  auto scope =
      instrumentationscope::InstrumentationScope::Create(name, version, schema_url, attrs_map);

  auto meter = std::shared_ptr<Meter>(new Meter(context_, std::move(scope)));
  context_->AddMeter(meter);
  return nostd::shared_ptr<metrics_api::Meter>{meter};
}

#if OPENTELEMETRY_ABI_VERSION_NO >= 2
void MeterProvider::RemoveMeter(nostd::string_view name,
                                nostd::string_view version,
                                nostd::string_view schema_url) noexcept
{
  if (name.data() == nullptr || name == "")
  {
    OTEL_INTERNAL_LOG_WARN("[MeterProvider::RemoveMeter] Library name is empty.");
    name = "";
  }

  const std::lock_guard<std::mutex> guard(lock_);

  context_->RemoveMeter(name, version, schema_url);
}
#endif

const resource::Resource &MeterProvider::GetResource() const noexcept
{
  return context_->GetResource();
}

void MeterProvider::AddMetricReader(std::shared_ptr<MetricReader> reader) noexcept
{
  context_->AddMetricReader(std::move(reader));
}

void MeterProvider::AddView(std::unique_ptr<InstrumentSelector> instrument_selector,
                            std::unique_ptr<MeterSelector> meter_selector,
                            std::unique_ptr<View> view) noexcept
{
  context_->AddView(std::move(instrument_selector), std::move(meter_selector), std::move(view));
}

#ifdef ENABLE_METRICS_EXEMPLAR_PREVIEW

void MeterProvider::SetExemplarFilter(metrics::ExemplarFilterType exemplar_filter_type) noexcept
{
  context_->SetExemplarFilter(exemplar_filter_type);
}

#endif  // ENABLE_METRICS_EXEMPLAR_PREVIEW

/**
 * Shutdown the meter provider.
 */
bool MeterProvider::Shutdown() noexcept
{
  return context_->Shutdown();
}

/**
 * Force flush the meter provider.
 */
bool MeterProvider::ForceFlush(std::chrono::microseconds timeout) noexcept
{
  return context_->ForceFlush(timeout);
}

/**
 * Shutdown MeterContext when MeterProvider is destroyed.
 *
 */
MeterProvider::~MeterProvider()
{
  if (context_)
  {
    context_->Shutdown();
  }
}

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
