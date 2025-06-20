// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <ostream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "opentelemetry/common/spin_lock_mutex.h"
#include "opentelemetry/common/timestamp.h"
#include "opentelemetry/metrics/async_instruments.h"
#include "opentelemetry/metrics/noop.h"
#include "opentelemetry/metrics/observer_result.h"
#include "opentelemetry/metrics/sync_instruments.h"
#include "opentelemetry/nostd/shared_ptr.h"
#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/nostd/unique_ptr.h"
#include "third_party/opentelemetry-cpp/sdk/include/opentelemetry/sdk/common/global_log_handler.h"
#include "third_party/opentelemetry-cpp/sdk/include/opentelemetry/sdk/instrumentationscope/instrumentation_scope.h"
#include "third_party/opentelemetry-cpp/sdk/include/opentelemetry/sdk/metrics/async_instruments.h"
#include "third_party/opentelemetry-cpp/sdk/include/opentelemetry/sdk/metrics/data/metric_data.h"
#include "third_party/opentelemetry-cpp/sdk/include/opentelemetry/sdk/metrics/instruments.h"
#include "third_party/opentelemetry-cpp/sdk/include/opentelemetry/sdk/metrics/meter.h"
#include "third_party/opentelemetry-cpp/sdk/include/opentelemetry/sdk/metrics/meter_context.h"
#include "third_party/opentelemetry-cpp/sdk/include/opentelemetry/sdk/metrics/state/async_metric_storage.h"
#include "third_party/opentelemetry-cpp/sdk/include/opentelemetry/sdk/metrics/state/metric_collector.h"
#include "third_party/opentelemetry-cpp/sdk/include/opentelemetry/sdk/metrics/state/metric_storage.h"
#include "third_party/opentelemetry-cpp/sdk/include/opentelemetry/sdk/metrics/state/multi_metric_storage.h"
#include "third_party/opentelemetry-cpp/sdk/include/opentelemetry/sdk/metrics/state/observable_registry.h"
#include "third_party/opentelemetry-cpp/sdk/include/opentelemetry/sdk/metrics/state/sync_metric_storage.h"
#include "third_party/opentelemetry-cpp/sdk/include/opentelemetry/sdk/metrics/sync_instruments.h"
#include "third_party/opentelemetry-cpp/sdk/include/opentelemetry/sdk/metrics/view/view.h"
#include "third_party/opentelemetry-cpp/sdk/include/opentelemetry/sdk/metrics/view/view_registry.h"
#include "third_party/opentelemetry-cpp/api/include/opentelemetry/version.h"

#ifdef ENABLE_METRICS_EXEMPLAR_PREVIEW
#  include "opentelemetry/sdk/metrics/exemplar/reservoir_utils.h"
#endif

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{

namespace metrics = opentelemetry::metrics;

Meter::Meter(
    std::weak_ptr<MeterContext> meter_context,
    std::unique_ptr<sdk::instrumentationscope::InstrumentationScope> instrumentation_scope) noexcept
    : scope_{std::move(instrumentation_scope)},
      meter_context_{std::move(meter_context)},
      observable_registry_(new ObservableRegistry())
{}

opentelemetry::nostd::unique_ptr<metrics::Counter<uint64_t>> Meter::CreateUInt64Counter(
    opentelemetry::nostd::string_view name,
    opentelemetry::nostd::string_view description,
    opentelemetry::nostd::string_view unit) noexcept
{
  if (!ValidateInstrument(name, description, unit))
  {
    OTEL_INTERNAL_LOG_ERROR("Meter::CreateUInt64Counter - failed. Invalid parameters."
                            << name << " " << description << " " << unit
                            << ". Measurements won't be recorded.");
    return opentelemetry::nostd::unique_ptr<metrics::Counter<uint64_t>>(
        new metrics::NoopCounter<uint64_t>(name, description, unit));
  }
  InstrumentDescriptor instrument_descriptor = {
      std::string{name.data(), name.size()}, std::string{description.data(), description.size()},
      std::string{unit.data(), unit.size()}, InstrumentType::kCounter, InstrumentValueType::kLong};
  auto storage = RegisterSyncMetricStorage(instrument_descriptor);
  return opentelemetry::nostd::unique_ptr<metrics::Counter<uint64_t>>(
      new LongCounter(instrument_descriptor, std::move(storage)));
}

opentelemetry::nostd::unique_ptr<metrics::Counter<double>> Meter::CreateDoubleCounter(
    opentelemetry::nostd::string_view name,
    opentelemetry::nostd::string_view description,
    opentelemetry::nostd::string_view unit) noexcept
{
  if (!ValidateInstrument(name, description, unit))
  {
    OTEL_INTERNAL_LOG_ERROR("Meter::CreateDoubleCounter - failed. Invalid parameters."
                            << name << " " << description << " " << unit
                            << ". Measurements won't be recorded.");
    return opentelemetry::nostd::unique_ptr<metrics::Counter<double>>(
        new metrics::NoopCounter<double>(name, description, unit));
  }
  InstrumentDescriptor instrument_descriptor = {
      std::string{name.data(), name.size()}, std::string{description.data(), description.size()},
      std::string{unit.data(), unit.size()}, InstrumentType::kCounter,
      InstrumentValueType::kDouble};
  auto storage = RegisterSyncMetricStorage(instrument_descriptor);
  return opentelemetry::nostd::unique_ptr<metrics::Counter<double>>{
      new DoubleCounter(instrument_descriptor, std::move(storage))};
}

opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObservableInstrument>
Meter::CreateInt64ObservableCounter(opentelemetry::nostd::string_view name,
                                    opentelemetry::nostd::string_view description,
                                    opentelemetry::nostd::string_view unit) noexcept
{
  if (!ValidateInstrument(name, description, unit))
  {
    OTEL_INTERNAL_LOG_ERROR("Meter::CreateInt64ObservableCounter - failed. Invalid parameters."
                            << name << " " << description << " " << unit
                            << ". Measurements won't be recorded.");
    return GetNoopObservableInsrument();
  }
  InstrumentDescriptor instrument_descriptor = {
      std::string{name.data(), name.size()}, std::string{description.data(), description.size()},
      std::string{unit.data(), unit.size()}, InstrumentType::kObservableCounter,
      InstrumentValueType::kLong};
  auto storage = RegisterAsyncMetricStorage(instrument_descriptor);
  return opentelemetry::nostd::shared_ptr<metrics::ObservableInstrument>{
      new ObservableInstrument(instrument_descriptor, std::move(storage), observable_registry_)};
}

opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObservableInstrument>
Meter::CreateDoubleObservableCounter(opentelemetry::nostd::string_view name,
                                     opentelemetry::nostd::string_view description,
                                     opentelemetry::nostd::string_view unit) noexcept
{
  if (!ValidateInstrument(name, description, unit))
  {
    OTEL_INTERNAL_LOG_ERROR("Meter::CreateDoubleObservableCounter - failed. Invalid parameters."
                            << name << " " << description << " " << unit
                            << ". Measurements won't be recorded.");
    return GetNoopObservableInsrument();
  }
  InstrumentDescriptor instrument_descriptor = {
      std::string{name.data(), name.size()}, std::string{description.data(), description.size()},
      std::string{unit.data(), unit.size()}, InstrumentType::kObservableCounter,
      InstrumentValueType::kDouble};
  auto storage = RegisterAsyncMetricStorage(instrument_descriptor);
  return opentelemetry::nostd::shared_ptr<metrics::ObservableInstrument>{
      new ObservableInstrument(instrument_descriptor, std::move(storage), observable_registry_)};
}

opentelemetry::nostd::unique_ptr<metrics::Histogram<uint64_t>> Meter::CreateUInt64Histogram(
    opentelemetry::nostd::string_view name,
    opentelemetry::nostd::string_view description,
    opentelemetry::nostd::string_view unit) noexcept
{
  if (!ValidateInstrument(name, description, unit))
  {
    OTEL_INTERNAL_LOG_ERROR("Meter::CreateUInt64Histogram - failed. Invalid parameters."
                            << name << " " << description << " " << unit
                            << ". Measurements won't be recorded.");
    return opentelemetry::nostd::unique_ptr<metrics::Histogram<uint64_t>>(
        new metrics::NoopHistogram<uint64_t>(name, description, unit));
  }
  InstrumentDescriptor instrument_descriptor = {
      std::string{name.data(), name.size()}, std::string{description.data(), description.size()},
      std::string{unit.data(), unit.size()}, InstrumentType::kHistogram,
      InstrumentValueType::kLong};
  auto storage = RegisterSyncMetricStorage(instrument_descriptor);
  return opentelemetry::nostd::unique_ptr<metrics::Histogram<uint64_t>>{
      new LongHistogram(instrument_descriptor, std::move(storage))};
}

opentelemetry::nostd::unique_ptr<metrics::Histogram<double>> Meter::CreateDoubleHistogram(
    opentelemetry::nostd::string_view name,
    opentelemetry::nostd::string_view description,
    opentelemetry::nostd::string_view unit) noexcept
{
  if (!ValidateInstrument(name, description, unit))
  {
    OTEL_INTERNAL_LOG_ERROR("Meter::CreateDoubleHistogram - failed. Invalid parameters."
                            << name << " " << description << " " << unit
                            << ". Measurements won't be recorded.");
    return opentelemetry::nostd::unique_ptr<metrics::Histogram<double>>(
        new metrics::NoopHistogram<double>(name, description, unit));
  }
  InstrumentDescriptor instrument_descriptor = {
      std::string{name.data(), name.size()}, std::string{description.data(), description.size()},
      std::string{unit.data(), unit.size()}, InstrumentType::kHistogram,
      InstrumentValueType::kDouble};
  auto storage = RegisterSyncMetricStorage(instrument_descriptor);
  return opentelemetry::nostd::unique_ptr<metrics::Histogram<double>>{
      new DoubleHistogram(instrument_descriptor, std::move(storage))};
}

opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObservableInstrument>
Meter::CreateInt64ObservableGauge(opentelemetry::nostd::string_view name,
                                  opentelemetry::nostd::string_view description,
                                  opentelemetry::nostd::string_view unit) noexcept
{
  if (!ValidateInstrument(name, description, unit))
  {
    OTEL_INTERNAL_LOG_ERROR("Meter::CreateInt64ObservableGauge - failed. Invalid parameters."
                            << name << " " << description << " " << unit
                            << ". Measurements won't be recorded.");
    return GetNoopObservableInsrument();
  }
  InstrumentDescriptor instrument_descriptor = {
      std::string{name.data(), name.size()}, std::string{description.data(), description.size()},
      std::string{unit.data(), unit.size()}, InstrumentType::kObservableGauge,
      InstrumentValueType::kLong};
  auto storage = RegisterAsyncMetricStorage(instrument_descriptor);
  return opentelemetry::nostd::shared_ptr<metrics::ObservableInstrument>{
      new ObservableInstrument(instrument_descriptor, std::move(storage), observable_registry_)};
}

opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObservableInstrument>
Meter::CreateDoubleObservableGauge(opentelemetry::nostd::string_view name,
                                   opentelemetry::nostd::string_view description,
                                   opentelemetry::nostd::string_view unit) noexcept
{
  if (!ValidateInstrument(name, description, unit))
  {
    OTEL_INTERNAL_LOG_ERROR("Meter::CreateDoubleObservableGauge - failed. Invalid parameters."
                            << name << " " << description << " " << unit
                            << ". Measurements won't be recorded.");
    return GetNoopObservableInsrument();
  }
  InstrumentDescriptor instrument_descriptor = {
      std::string{name.data(), name.size()}, std::string{description.data(), description.size()},
      std::string{unit.data(), unit.size()}, InstrumentType::kObservableGauge,
      InstrumentValueType::kDouble};
  auto storage = RegisterAsyncMetricStorage(instrument_descriptor);
  return opentelemetry::nostd::shared_ptr<metrics::ObservableInstrument>{
      new ObservableInstrument(instrument_descriptor, std::move(storage), observable_registry_)};
}

opentelemetry::nostd::unique_ptr<metrics::UpDownCounter<int64_t>> Meter::CreateInt64UpDownCounter(
    opentelemetry::nostd::string_view name,
    opentelemetry::nostd::string_view description,
    opentelemetry::nostd::string_view unit) noexcept
{
  if (!ValidateInstrument(name, description, unit))
  {
    OTEL_INTERNAL_LOG_ERROR("Meter::CreateInt64UpDownCounter - failed. Invalid parameters."
                            << name << " " << description << " " << unit
                            << ". Measurements won't be recorded.");
    return opentelemetry::nostd::unique_ptr<metrics::UpDownCounter<int64_t>>(
        new metrics::NoopUpDownCounter<int64_t>(name, description, unit));
  }
  InstrumentDescriptor instrument_descriptor = {
      std::string{name.data(), name.size()}, std::string{description.data(), description.size()},
      std::string{unit.data(), unit.size()}, InstrumentType::kUpDownCounter,
      InstrumentValueType::kLong};
  auto storage = RegisterSyncMetricStorage(instrument_descriptor);
  return opentelemetry::nostd::unique_ptr<metrics::UpDownCounter<int64_t>>{
      new LongUpDownCounter(instrument_descriptor, std::move(storage))};
}

opentelemetry::nostd::unique_ptr<metrics::UpDownCounter<double>> Meter::CreateDoubleUpDownCounter(
    opentelemetry::nostd::string_view name,
    opentelemetry::nostd::string_view description,
    opentelemetry::nostd::string_view unit) noexcept
{
  if (!ValidateInstrument(name, description, unit))
  {
    OTEL_INTERNAL_LOG_ERROR("Meter::CreateDoubleUpDownCounter - failed. Invalid parameters."
                            << name << " " << description << " " << unit
                            << ". Measurements won't be recorded.");
    return opentelemetry::nostd::unique_ptr<metrics::UpDownCounter<double>>(
        new metrics::NoopUpDownCounter<double>(name, description, unit));
  }
  InstrumentDescriptor instrument_descriptor = {
      std::string{name.data(), name.size()}, std::string{description.data(), description.size()},
      std::string{unit.data(), unit.size()}, InstrumentType::kUpDownCounter,
      InstrumentValueType::kDouble};
  auto storage = RegisterSyncMetricStorage(instrument_descriptor);
  return opentelemetry::nostd::unique_ptr<metrics::UpDownCounter<double>>{
      new DoubleUpDownCounter(instrument_descriptor, std::move(storage))};
}

opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObservableInstrument>
Meter::CreateInt64ObservableUpDownCounter(opentelemetry::nostd::string_view name,
                                          opentelemetry::nostd::string_view description,
                                          opentelemetry::nostd::string_view unit) noexcept
{
  if (!ValidateInstrument(name, description, unit))
  {
    OTEL_INTERNAL_LOG_ERROR(
        "Meter::CreateInt64ObservableUpDownCounter - failed. Invalid parameters -"
        << name << " " << description << " " << unit << ". Measurements won't be recorded.");
    return GetNoopObservableInsrument();
  }
  InstrumentDescriptor instrument_descriptor = {
      std::string{name.data(), name.size()}, std::string{description.data(), description.size()},
      std::string{unit.data(), unit.size()}, InstrumentType::kObservableUpDownCounter,
      InstrumentValueType::kLong};
  auto storage = RegisterAsyncMetricStorage(instrument_descriptor);
  return opentelemetry::nostd::shared_ptr<metrics::ObservableInstrument>{
      new ObservableInstrument(instrument_descriptor, std::move(storage), observable_registry_)};
}

opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObservableInstrument>
Meter::CreateDoubleObservableUpDownCounter(opentelemetry::nostd::string_view name,
                                           opentelemetry::nostd::string_view description,
                                           opentelemetry::nostd::string_view unit) noexcept
{
  if (!ValidateInstrument(name, description, unit))
  {
    OTEL_INTERNAL_LOG_ERROR(
        "Meter::CreateDoubleObservableUpDownCounter - failed. Invalid parameters."
        << name << " " << description << " " << unit << ". Measurements won't be recorded.");
    return GetNoopObservableInsrument();
  }
  InstrumentDescriptor instrument_descriptor = {
      std::string{name.data(), name.size()}, std::string{description.data(), description.size()},
      std::string{unit.data(), unit.size()}, InstrumentType::kObservableUpDownCounter,
      InstrumentValueType::kDouble};
  auto storage = RegisterAsyncMetricStorage(instrument_descriptor);
  return opentelemetry::nostd::shared_ptr<metrics::ObservableInstrument>{
      new ObservableInstrument(instrument_descriptor, std::move(storage), observable_registry_)};
}

const sdk::instrumentationscope::InstrumentationScope *Meter::GetInstrumentationScope()
    const noexcept
{
  return scope_.get();
}

std::unique_ptr<SyncWritableMetricStorage> Meter::RegisterSyncMetricStorage(
    InstrumentDescriptor &instrument_descriptor)
{
  std::lock_guard<opentelemetry::common::SpinLockMutex> guard(storage_lock_);
  auto ctx = meter_context_.lock();
  if (!ctx)
  {
    OTEL_INTERNAL_LOG_ERROR(
        "[Meter::RegisterSyncMetricStorage] - Error during finding matching views."
        << "The metric context is invalid");
    return nullptr;
  }

  auto view_registry = ctx->GetViewRegistry();
  std::unique_ptr<SyncWritableMetricStorage> storages(new SyncMultiMetricStorage());

#ifdef ENABLE_METRICS_EXEMPLAR_PREVIEW
  auto exemplar_filter_type = ctx->GetExemplarFilter();
#endif

  auto success = view_registry->FindViews(
      instrument_descriptor, *scope_,
      [this, &instrument_descriptor, &storages
#ifdef ENABLE_METRICS_EXEMPLAR_PREVIEW
       ,
       exemplar_filter_type
#endif
  ](const View &view) {
        auto view_instr_desc = instrument_descriptor;
        if (!view.GetName().empty())
        {
          view_instr_desc.name_ = view.GetName();
        }
        if (!view.GetDescription().empty())
        {
          view_instr_desc.description_ = view.GetDescription();
        }
        auto multi_storage = static_cast<SyncMultiMetricStorage *>(storages.get());

        auto storage = std::shared_ptr<SyncMetricStorage>(new SyncMetricStorage(
            view_instr_desc, view.GetAggregationType(), &view.GetAttributesProcessor(),
#ifdef ENABLE_METRICS_EXEMPLAR_PREVIEW
            exemplar_filter_type,
            GetExemplarReservoir(view.GetAggregationType(), view.GetAggregationConfig(),
                                 instrument_descriptor),
#endif
            view.GetAggregationConfig()));
        storage_registry_[instrument_descriptor.name_] = storage;
        multi_storage->AddStorage(storage);
        return true;
      });

  if (!success)
  {
    OTEL_INTERNAL_LOG_ERROR(
        "[Meter::RegisterSyncMetricStorage] - Error during finding matching views."
        << "Some of the matching view configurations mayn't be used for metric collection");
  }
  return storages;
}

std::unique_ptr<AsyncWritableMetricStorage> Meter::RegisterAsyncMetricStorage(
    InstrumentDescriptor &instrument_descriptor)
{
  std::lock_guard<opentelemetry::common::SpinLockMutex> guard(storage_lock_);
  auto ctx = meter_context_.lock();
  if (!ctx)
  {
    OTEL_INTERNAL_LOG_ERROR(
        "[Meter::RegisterAsyncMetricStorage] - Error during finding matching views."
        << "The metric context is invalid");
    return nullptr;
  }
  auto view_registry = ctx->GetViewRegistry();
  std::unique_ptr<AsyncWritableMetricStorage> storages(new AsyncMultiMetricStorage());

#ifdef ENABLE_METRICS_EXEMPLAR_PREVIEW
  auto exemplar_filter_type = ctx->GetExemplarFilter();
#endif

  auto success = view_registry->FindViews(
      instrument_descriptor, *GetInstrumentationScope(),
      [this, &instrument_descriptor, &storages
#ifdef ENABLE_METRICS_EXEMPLAR_PREVIEW
       ,
       exemplar_filter_type
#endif
  ](const View &view) {
        auto view_instr_desc = instrument_descriptor;
        if (!view.GetName().empty())
        {
          view_instr_desc.name_ = view.GetName();
        }
        if (!view.GetDescription().empty())
        {
          view_instr_desc.description_ = view.GetDescription();
        }
        auto storage = std::shared_ptr<AsyncMetricStorage>(new AsyncMetricStorage(
            view_instr_desc, view.GetAggregationType(),
#ifdef ENABLE_METRICS_EXEMPLAR_PREVIEW
            exemplar_filter_type,
            GetExemplarReservoir(view.GetAggregationType(), view.GetAggregationConfig(),
                                 instrument_descriptor),
#endif
            view.GetAggregationConfig()));
        storage_registry_[instrument_descriptor.name_] = storage;
        static_cast<AsyncMultiMetricStorage *>(storages.get())->AddStorage(storage);
        return true;
      });
  if (!success)
  {
    OTEL_INTERNAL_LOG_ERROR(
        "[Meter::RegisterAsyncMetricStorage] - Error during finding matching views."
        << "Some of the matching view configurations mayn't be used for metric collection");
  }
  return storages;
}

/** collect metrics across all the meters **/
std::vector<MetricData> Meter::Collect(CollectorHandle *collector,
                                       opentelemetry::common::SystemTimestamp collect_ts) noexcept
{
  observable_registry_->Observe(collect_ts);
  std::vector<MetricData> metric_data_list;
  auto ctx = meter_context_.lock();
  if (!ctx)
  {
    OTEL_INTERNAL_LOG_ERROR("[Meter::Collect] - Error during collection."
                            << "The metric context is invalid");
    return std::vector<MetricData>{};
  }
  std::lock_guard<opentelemetry::common::SpinLockMutex> guard(storage_lock_);
  for (auto &metric_storage : storage_registry_)
  {
    metric_storage.second->Collect(collector, ctx->GetCollectors(), ctx->GetSDKStartTime(),
                                   collect_ts, [&metric_data_list](const MetricData &metric_data) {
                                     metric_data_list.push_back(metric_data);
                                     return true;
                                   });
  }
  return metric_data_list;
}

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
