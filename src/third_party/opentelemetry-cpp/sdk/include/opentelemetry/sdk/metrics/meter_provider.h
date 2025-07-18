// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <memory>
#include <mutex>

#include "opentelemetry/metrics/meter.h"
#include "opentelemetry/metrics/meter_provider.h"
#include "opentelemetry/nostd/shared_ptr.h"
#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/sdk/metrics/meter_context.h"
#include "opentelemetry/sdk/metrics/metric_reader.h"
#include "opentelemetry/sdk/metrics/view/instrument_selector.h"
#include "opentelemetry/sdk/metrics/view/meter_selector.h"
#include "opentelemetry/sdk/metrics/view/view.h"
#include "opentelemetry/sdk/metrics/view/view_registry.h"
#include "opentelemetry/sdk/resource/resource.h"
#include "opentelemetry/version.h"

#ifdef ENABLE_METRICS_EXEMPLAR_PREVIEW
#  include "opentelemetry/sdk/metrics/exemplar/filter_type.h"
#endif

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{

class OPENTELEMETRY_EXPORT MeterProvider final : public opentelemetry::metrics::MeterProvider
{
public:
  /**
   * Initialize a new meter provider
   * @param views The views for this meter provider
   * @param resource  The resources for this meter provider.
   */
  MeterProvider(
      std::unique_ptr<ViewRegistry> views     = std::unique_ptr<ViewRegistry>(new ViewRegistry()),
      const sdk::resource::Resource &resource = sdk::resource::Resource::Create({})) noexcept;

  /**
   * Initialize a new meter provider with a specified context
   * @param context The owned meter configuration/pipeline for this provider.
   */
  explicit MeterProvider(std::unique_ptr<MeterContext> context) noexcept;

  /*
    Make sure GetMeter() helpers from the API are seen in overload resolution.
  */
  using opentelemetry::metrics::MeterProvider::GetMeter;

#if OPENTELEMETRY_ABI_VERSION_NO >= 2
  nostd::shared_ptr<opentelemetry::metrics::Meter> GetMeter(
      nostd::string_view name,
      nostd::string_view version,
      nostd::string_view schema_url,
      const opentelemetry::common::KeyValueIterable *attributes) noexcept override;
#else
  nostd::shared_ptr<opentelemetry::metrics::Meter> GetMeter(
      nostd::string_view name,
      nostd::string_view version    = "",
      nostd::string_view schema_url = "") noexcept override;
#endif

#if OPENTELEMETRY_ABI_VERSION_NO >= 2
  void RemoveMeter(nostd::string_view name,
                   nostd::string_view version,
                   nostd::string_view schema_url) noexcept override;
#endif

  /**
   * Obtain the resource associated with this meter provider.
   * @return The resource for this meter provider.
   */
  const sdk::resource::Resource &GetResource() const noexcept;

  /**
   * Attaches a metric reader to list of configured readers for this Meter providers.
   * @param reader The metric reader for this meter provider. This
   * must not be a nullptr.
   *
   * Note: This reader may not receive any in-flight meter data, but will get newly created meter
   * data. Note: This method is not thread safe, and should ideally be called from main thread.
   */
  void AddMetricReader(std::shared_ptr<MetricReader> reader) noexcept;

  /**
   * Attaches a View to list of configured Views for this Meter provider.
   * @param view The Views for this meter provider. This
   * must not be a nullptr.
   *
   * Note: This view may not receive any in-flight meter data, but will get newly created meter
   * data. Note: This method is not thread safe, and should ideally be called from main thread.
   */
  void AddView(std::unique_ptr<InstrumentSelector> instrument_selector,
               std::unique_ptr<MeterSelector> meter_selector,
               std::unique_ptr<View> view) noexcept;

#ifdef ENABLE_METRICS_EXEMPLAR_PREVIEW

  void SetExemplarFilter(metrics::ExemplarFilterType exemplar_filter_type =
                             metrics::ExemplarFilterType::kTraceBased) noexcept;

#endif

  /**
   * Shutdown the meter provider.
   */
  bool Shutdown() noexcept;

  /**
   * Force flush the meter provider.
   */
  bool ForceFlush(std::chrono::microseconds timeout = (std::chrono::microseconds::max)()) noexcept;

  ~MeterProvider() override;

private:
  std::shared_ptr<MeterContext> context_;
  std::mutex lock_;
};
}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
