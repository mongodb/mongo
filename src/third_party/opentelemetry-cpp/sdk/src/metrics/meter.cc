// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

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
#include "opentelemetry/metrics/sync_instruments.h"
#include "opentelemetry/nostd/function_ref.h"
#include "opentelemetry/nostd/shared_ptr.h"
#include "opentelemetry/nostd/span.h"
#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/nostd/unique_ptr.h"
#include "opentelemetry/sdk/common/global_log_handler.h"
#include "opentelemetry/sdk/instrumentationscope/instrumentation_scope.h"
#include "opentelemetry/sdk/instrumentationscope/scope_configurator.h"
#include "opentelemetry/sdk/metrics/async_instruments.h"
#include "opentelemetry/sdk/metrics/data/metric_data.h"
#include "opentelemetry/sdk/metrics/instruments.h"
#include "opentelemetry/sdk/metrics/meter.h"
#include "opentelemetry/sdk/metrics/meter_config.h"
#include "opentelemetry/sdk/metrics/meter_context.h"
#include "opentelemetry/sdk/metrics/state/async_metric_storage.h"
#include "opentelemetry/sdk/metrics/state/metric_collector.h"
#include "opentelemetry/sdk/metrics/state/metric_storage.h"
#include "opentelemetry/sdk/metrics/state/multi_metric_storage.h"
#include "opentelemetry/sdk/metrics/state/observable_registry.h"
#include "opentelemetry/sdk/metrics/state/sync_metric_storage.h"
#include "opentelemetry/sdk/metrics/sync_instruments.h"
#include "opentelemetry/sdk/metrics/view/view.h"
#include "opentelemetry/sdk/metrics/view/view_registry.h"
#include "opentelemetry/version.h"

#ifdef ENABLE_METRICS_EXEMPLAR_PREVIEW
#  include "opentelemetry/sdk/metrics/exemplar/filter_type.h"
#  include "opentelemetry/sdk/metrics/exemplar/reservoir_utils.h"
#endif

#if OPENTELEMETRY_ABI_VERSION_NO >= 2
#  include "opentelemetry/metrics/meter.h"
#endif

namespace
{

struct InstrumentationScopeLogStreamable
{
  const opentelemetry::sdk::instrumentationscope::InstrumentationScope &scope;
};

struct InstrumentDescriptorLogStreamable
{
  const opentelemetry::sdk::metrics::InstrumentDescriptor &instrument;
};

std::ostream &operator<<(std::ostream &os,
                         const InstrumentationScopeLogStreamable &streamable) noexcept
{
  os << "\n  name=\"" << streamable.scope.GetName() << "\"" << "\n  schema_url=\""
     << streamable.scope.GetSchemaURL() << "\"" << "\n  version=\"" << streamable.scope.GetVersion()
     << "\"";
  return os;
}

std::ostream &operator<<(std::ostream &os,
                         const InstrumentDescriptorLogStreamable &streamable) noexcept
{
  os << "\n  name=\"" << streamable.instrument.name_ << "\"" << "\n  description=\""
     << streamable.instrument.description_ << "\"" << "\n  unit=\"" << streamable.instrument.unit_
     << "\"" << "\n  kind=\""
     << opentelemetry::sdk::metrics::InstrumentDescriptorUtil::GetInstrumentValueTypeString(
            streamable.instrument.value_type_)
     << opentelemetry::sdk::metrics::InstrumentDescriptorUtil::GetInstrumentTypeString(
            streamable.instrument.type_)
     << "\"";
  return os;
}

}  // namespace

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{

namespace metrics = opentelemetry::metrics;

metrics::NoopMeter Meter::kNoopMeter = metrics::NoopMeter();

Meter::Meter(
    std::weak_ptr<MeterContext> meter_context,
    std::unique_ptr<sdk::instrumentationscope::InstrumentationScope> instrumentation_scope) noexcept
    : scope_{std::move(instrumentation_scope)},
      meter_context_{std::move(meter_context)},
      observable_registry_(new ObservableRegistry()),
      meter_config_(MeterConfig::Default())
{
  if (auto meter_context_locked_ptr = meter_context_.lock())
  {
    meter_config_ = meter_context_locked_ptr->GetMeterConfigurator().ComputeConfig(*scope_);
  }
  else
  {
    OTEL_INTERNAL_LOG_ERROR("[Meter::Meter()] - Error during initialization."
                            << "The metric context is invalid")
  }
}

opentelemetry::nostd::unique_ptr<metrics::Counter<uint64_t>> Meter::CreateUInt64Counter(
    opentelemetry::nostd::string_view name,
    opentelemetry::nostd::string_view description,
    opentelemetry::nostd::string_view unit) noexcept
{
  if (!meter_config_.IsEnabled())
  {
    return kNoopMeter.CreateUInt64Counter(name, description, unit);
  }

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
  if (!meter_config_.IsEnabled())
  {
    return kNoopMeter.CreateDoubleCounter(name, description, unit);
  }

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
  if (!meter_config_.IsEnabled())
  {
    return kNoopMeter.CreateInt64ObservableCounter(name, description, unit);
  }

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
  if (!meter_config_.IsEnabled())
  {
    return kNoopMeter.CreateDoubleObservableCounter(name, description, unit);
  }

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
  if (!meter_config_.IsEnabled())
  {
    return kNoopMeter.CreateUInt64Histogram(name, description, unit);
  }

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
  if (!meter_config_.IsEnabled())
  {
    return kNoopMeter.CreateDoubleHistogram(name, description, unit);
  }

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

#if OPENTELEMETRY_ABI_VERSION_NO >= 2
opentelemetry::nostd::unique_ptr<metrics::Gauge<int64_t>> Meter::CreateInt64Gauge(
    opentelemetry::nostd::string_view name,
    opentelemetry::nostd::string_view description,
    opentelemetry::nostd::string_view unit) noexcept
{
  if (!meter_config_.IsEnabled())
  {
    return kNoopMeter.CreateInt64Gauge(name, description, unit);
  }

  if (!ValidateInstrument(name, description, unit))
  {
    OTEL_INTERNAL_LOG_ERROR("Meter::CreateInt64Gauge - failed. Invalid parameters."
                            << name << " " << description << " " << unit
                            << ". Measurements won't be recorded.");
    return opentelemetry::nostd::unique_ptr<metrics::Gauge<int64_t>>(
        new metrics::NoopGauge<int64_t>(name, description, unit));
  }
  InstrumentDescriptor instrument_descriptor = {
      std::string{name.data(), name.size()}, std::string{description.data(), description.size()},
      std::string{unit.data(), unit.size()}, InstrumentType::kGauge, InstrumentValueType::kLong};
  auto storage = RegisterSyncMetricStorage(instrument_descriptor);
  return opentelemetry::nostd::unique_ptr<metrics::Gauge<int64_t>>{
      new LongGauge(instrument_descriptor, std::move(storage))};
}

opentelemetry::nostd::unique_ptr<metrics::Gauge<double>> Meter::CreateDoubleGauge(
    opentelemetry::nostd::string_view name,
    opentelemetry::nostd::string_view description,
    opentelemetry::nostd::string_view unit) noexcept
{
  if (!meter_config_.IsEnabled())
  {
    return kNoopMeter.CreateDoubleGauge(name, description, unit);
  }

  if (!ValidateInstrument(name, description, unit))
  {
    OTEL_INTERNAL_LOG_ERROR("Meter::CreateDoubleGauge - failed. Invalid parameters."
                            << name << " " << description << " " << unit
                            << ". Measurements won't be recorded.");
    return opentelemetry::nostd::unique_ptr<metrics::Gauge<double>>(
        new metrics::NoopGauge<double>(name, description, unit));
  }
  InstrumentDescriptor instrument_descriptor = {
      std::string{name.data(), name.size()}, std::string{description.data(), description.size()},
      std::string{unit.data(), unit.size()}, InstrumentType::kGauge, InstrumentValueType::kDouble};
  auto storage = RegisterSyncMetricStorage(instrument_descriptor);
  return opentelemetry::nostd::unique_ptr<metrics::Gauge<double>>{
      new DoubleGauge(instrument_descriptor, std::move(storage))};
}
#endif

opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObservableInstrument>
Meter::CreateInt64ObservableGauge(opentelemetry::nostd::string_view name,
                                  opentelemetry::nostd::string_view description,
                                  opentelemetry::nostd::string_view unit) noexcept
{
  if (!meter_config_.IsEnabled())
  {
    return kNoopMeter.CreateInt64ObservableGauge(name, description, unit);
  }

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
  if (!meter_config_.IsEnabled())
  {
    return kNoopMeter.CreateDoubleObservableGauge(name, description, unit);
  }

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
  if (!meter_config_.IsEnabled())
  {
    return kNoopMeter.CreateInt64UpDownCounter(name, description, unit);
  }

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
  if (!meter_config_.IsEnabled())
  {
    return kNoopMeter.CreateDoubleUpDownCounter(name, description, unit);
  }

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
  if (!meter_config_.IsEnabled())
  {
    return kNoopMeter.CreateInt64ObservableUpDownCounter(name, description, unit);
  }

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
  if (!meter_config_.IsEnabled())
  {
    return kNoopMeter.CreateDoubleObservableUpDownCounter(name, description, unit);
  }

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
        std::shared_ptr<SyncMetricStorage> sync_storage{};
        auto storage_iter = storage_registry_.find(view_instr_desc);
        if (storage_iter != storage_registry_.end())
        {
          WarnOnNameCaseConflict(GetInstrumentationScope(), storage_iter->first, view_instr_desc);
          // static_pointer_cast is okay here. If storage_registry_.find is successful
          // InstrumentEqualNameCaseInsensitive ensures that the
          // instrument type and value type are the same for the existing and new instrument.
          sync_storage = std::static_pointer_cast<SyncMetricStorage>(storage_iter->second);
        }
        else
        {
          WarnOnDuplicateInstrument(GetInstrumentationScope(), storage_registry_, view_instr_desc);
          sync_storage = std::shared_ptr<SyncMetricStorage>(new SyncMetricStorage(
              view_instr_desc, view.GetAggregationType(), view.GetAttributesProcessor(),
#ifdef ENABLE_METRICS_EXEMPLAR_PREVIEW
              exemplar_filter_type,
              GetExemplarReservoir(view.GetAggregationType(), view.GetAggregationConfig(),
                                   view_instr_desc),
#endif
              view.GetAggregationConfig()));
          storage_registry_.insert({view_instr_desc, sync_storage});
        }
        auto sync_multi_storage = static_cast<SyncMultiMetricStorage *>(storages.get());
        sync_multi_storage->AddStorage(sync_storage);
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
        std::shared_ptr<AsyncMetricStorage> async_storage{};
        auto storage_iter = storage_registry_.find(view_instr_desc);
        if (storage_iter != storage_registry_.end())
        {
          WarnOnNameCaseConflict(GetInstrumentationScope(), storage_iter->first, view_instr_desc);
          // static_pointer_cast is okay here. If storage_registry_.find is successful
          // InstrumentEqualNameCaseInsensitive ensures that the
          // instrument type and value type are the same for the existing and new instrument.
          async_storage = std::static_pointer_cast<AsyncMetricStorage>(storage_iter->second);
        }
        else
        {
          WarnOnDuplicateInstrument(GetInstrumentationScope(), storage_registry_, view_instr_desc);
          async_storage = std::shared_ptr<AsyncMetricStorage>(new AsyncMetricStorage(
              view_instr_desc, view.GetAggregationType(),
#ifdef ENABLE_METRICS_EXEMPLAR_PREVIEW
              exemplar_filter_type,
              GetExemplarReservoir(view.GetAggregationType(), view.GetAggregationConfig(),
                                   view_instr_desc),
#endif
              view.GetAggregationConfig()));
          storage_registry_.insert({view_instr_desc, async_storage});
        }
        auto async_multi_storage = static_cast<AsyncMultiMetricStorage *>(storages.get());
        async_multi_storage->AddStorage(async_storage);
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
  if (!meter_config_.IsEnabled())
  {
    return std::vector<MetricData>();
  }
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

#if OPENTELEMETRY_ABI_VERSION_NO >= 2
uintptr_t Meter::RegisterCallback(
    opentelemetry::metrics::MultiObservableCallbackPtr callback,
    void *state,
    nostd::span<opentelemetry::metrics::ObservableInstrument *> instruments) noexcept
{
  return observable_registry_->AddCallback(callback, state, instruments);
}

void Meter::DeregisterCallback(uintptr_t callback_id) noexcept
{
  observable_registry_->RemoveCallback(callback_id);
}
#endif

// Implementation of the log message recommended by the SDK specification for duplicate instruments.
// See
// https://github.com/open-telemetry/opentelemetry-specification/blob/9c8c30631b0e288de93df7452f91ed47f6fba330/specification/metrics/sdk.md?plain=1#L882
void Meter::WarnOnDuplicateInstrument(const sdk::instrumentationscope::InstrumentationScope *scope,
                                      const MetricStorageMap &storage_registry,
                                      const InstrumentDescriptor &new_instrument)
{
  for (const auto &element : storage_registry)
  {
    const auto &existing_instrument = element.first;
    if (InstrumentDescriptorUtil::IsDuplicate(existing_instrument, new_instrument))
    {
      std::string resolution_info{""};

      if (existing_instrument.type_ != new_instrument.type_ ||
          existing_instrument.value_type_ != new_instrument.value_type_)
      {
        resolution_info +=
            "\nDifferent instrument kinds found. Consider configuring a View to change the name of "
            "the duplicate instrument.";
      }

      if (existing_instrument.unit_ != new_instrument.unit_)
      {
        resolution_info += "\nDifferent instrument units found.";
      }

      if (existing_instrument.description_ != new_instrument.description_)
      {
        resolution_info +=
            "\nDifferent instrument descriptions found. Consider configuring a View to change the "
            "description of the duplicate instrument.";
      }

      OTEL_INTERNAL_LOG_WARN(
          "[Meter::WarnOnDuplicateInstrument] Creating a duplicate instrument of the same "
          "case-insensitive name. This may cause "
          "semantic errors in the data exported from this meter."
          << resolution_info << "\nScope: " << InstrumentationScopeLogStreamable{*scope}
          << "\nExisting instrument: " << InstrumentDescriptorLogStreamable{existing_instrument}
          << "\nDuplicate instrument: " << InstrumentDescriptorLogStreamable{new_instrument});
      return;
    }
  }
}

// Implementation of the log message recommended by the SDK specification for name case conflicts.
// See
// https://github.com/open-telemetry/opentelemetry-specification/blob/9c8c30631b0e288de93df7452f91ed47f6fba330/specification/metrics/sdk.md?plain=1#L910
void Meter::WarnOnNameCaseConflict(const sdk::instrumentationscope::InstrumentationScope *scope,
                                   const InstrumentDescriptor &existing_instrument,
                                   const InstrumentDescriptor &new_instrument)
{
  if (InstrumentDescriptorUtil::CaseInsensitiveAsciiEquals(existing_instrument.name_,
                                                           new_instrument.name_) &&
      existing_instrument.name_ != new_instrument.name_)
  {
    OTEL_INTERNAL_LOG_WARN(
        "[Meter::WarnOnNameCaseConflict] Instrument name case conflict detected on creation. "
        "Returning the existing instrument with the first-seen instrument name. To resolve this "
        "warning consider configuring a View to rename the duplicate instrument."
        << "\nScope: " << InstrumentationScopeLogStreamable{*scope}
        << "\nExisting instrument: " << InstrumentDescriptorLogStreamable{existing_instrument}
        << "\nDuplicate instrument: " << InstrumentDescriptorLogStreamable{new_instrument});
  }
}

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
