// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <vector>

#include "opentelemetry/common/spin_lock_mutex.h"
#include "opentelemetry/common/timestamp.h"
#include "opentelemetry/nostd/function_ref.h"
#include "opentelemetry/nostd/span.h"
#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/sdk/metrics/metric_reader.h"
#include "opentelemetry/sdk/metrics/state/metric_collector.h"
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

// forward declaration
class CollectorHandle;
class InstrumentSelector;
class Meter;
class MetricReader;
class MeterSelector;

/**
 * A class which stores the MeterProvider context.

 */
class MeterContext : public std::enable_shared_from_this<MeterContext>
{
public:
  /**
   * Initialize a new meter provider
   * @param readers The readers to be configured with meter context.
   * @param views The views to be configured with meter context.
   * @param resource  The resource for this meter context.
   */
  MeterContext(
      std::unique_ptr<ViewRegistry> views = std::unique_ptr<ViewRegistry>(new ViewRegistry()),
      const opentelemetry::sdk::resource::Resource &resource =
          opentelemetry::sdk::resource::Resource::Create({})) noexcept;

  /**
   * Obtain the resource associated with this meter context.
   * @return The resource for this meter context
   */
  const opentelemetry::sdk::resource::Resource &GetResource() const noexcept;

  /**
   * Obtain the View Registry configured
   * @return The reference to view registry
   */
  ViewRegistry *GetViewRegistry() const noexcept;

  /**
   * NOTE - INTERNAL method, can change in future.
   * Process callback for each meter in thread-safe manner
   */
  bool ForEachMeter(nostd::function_ref<bool(std::shared_ptr<Meter> &meter)> callback) noexcept;

  /**
   * NOTE - INTERNAL method, can change in future.
   * Get the configured meters.
   * This method is NOT thread safe, and only called through MeterProvider
   *
   */
  nostd::span<std::shared_ptr<Meter>> GetMeters() noexcept;

  /**
   * Obtain the configured collectors.
   *
   */
  nostd::span<std::shared_ptr<CollectorHandle>> GetCollectors() noexcept;

  /**
   * GET SDK Start time
   *
   */
  opentelemetry::common::SystemTimestamp GetSDKStartTime() noexcept;

  /**
   * Attaches a metric reader to list of configured readers for this Meter context.
   * @param reader The metric reader for this meter context. This
   * must not be a nullptr.
   *
   * Note: This reader may not receive any in-flight meter data, but will get newly created meter
   * data. Note: This method is not thread safe, and should ideally be called from main thread.
   */
  void AddMetricReader(std::shared_ptr<MetricReader> reader) noexcept;

  /**
   * Attaches a View to list of configured Views for this Meter context.
   * @param view The Views for this meter context. This
   * must not be a nullptr.
   *
   * Note: This view may not receive any in-flight meter data, but will get newly created meter
   * data. Note: This method is not thread safe, and should ideally be called from main thread.
   */
  void AddView(std::unique_ptr<InstrumentSelector> instrument_selector,
               std::unique_ptr<MeterSelector> meter_selector,
               std::unique_ptr<View> view) noexcept;

#ifdef ENABLE_METRICS_EXEMPLAR_PREVIEW

  void SetExemplarFilter(ExemplarFilterType exemplar_filter_type) noexcept;

  ExemplarFilterType GetExemplarFilter() const noexcept;

#endif

  /**
   * NOTE - INTERNAL method, can change in future.
   * Adds a meter to the list of configured meters in thread safe manner.
   *
   * @param meter
   */
  void AddMeter(const std::shared_ptr<Meter> &meter);

  void RemoveMeter(nostd::string_view name,
                   nostd::string_view version,
                   nostd::string_view schema_url);

  /**
   * Force all active Collectors to flush any buffered meter data
   * within the given timeout.
   */

  bool ForceFlush(std::chrono::microseconds timeout = (std::chrono::microseconds::max)()) noexcept;

  /**
   * Shutdown the Collectors associated with this meter provider.
   */
  bool Shutdown() noexcept;

private:
  opentelemetry::sdk::resource::Resource resource_;
  std::vector<std::shared_ptr<CollectorHandle>> collectors_;
  std::unique_ptr<ViewRegistry> views_;
  opentelemetry::common::SystemTimestamp sdk_start_ts_;
  std::vector<std::shared_ptr<Meter>> meters_;

#ifdef ENABLE_METRICS_EXEMPLAR_PREVIEW
  metrics::ExemplarFilterType exemplar_filter_type_ = metrics::ExemplarFilterType::kAlwaysOff;
#endif

#if defined(__cpp_lib_atomic_value_initialization) && \
    __cpp_lib_atomic_value_initialization >= 201911L
  std::atomic_flag shutdown_latch_{};
#else
  std::atomic_flag shutdown_latch_ = ATOMIC_FLAG_INIT;
#endif
  opentelemetry::common::SpinLockMutex forceflush_lock_;
  opentelemetry::common::SpinLockMutex meter_lock_;
};

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
