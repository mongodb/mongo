// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <memory>
#include <utility>

#include "opentelemetry/metrics/async_instruments.h"
#include "opentelemetry/sdk/metrics/async_instruments.h"
#include "opentelemetry/sdk/metrics/instruments.h"
#include "opentelemetry/sdk/metrics/state/metric_storage.h"
#include "opentelemetry/sdk/metrics/state/observable_registry.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{

ObservableInstrument::ObservableInstrument(InstrumentDescriptor instrument_descriptor,
                                           std::unique_ptr<AsyncWritableMetricStorage> storage,
                                           std::shared_ptr<ObservableRegistry> observable_registry)
    : instrument_descriptor_(std::move(instrument_descriptor)),
      storage_(std::move(storage)),
      observable_registry_{std::move(observable_registry)}

{}

ObservableInstrument::~ObservableInstrument()
{
  observable_registry_->CleanupCallback(this);
}

void ObservableInstrument::AddCallback(opentelemetry::metrics::ObservableCallbackPtr callback,
                                       void *state) noexcept
{
  observable_registry_->AddCallback(callback, state, this);
}

void ObservableInstrument::RemoveCallback(opentelemetry::metrics::ObservableCallbackPtr callback,
                                          void *state) noexcept
{
  observable_registry_->RemoveCallback(callback, state, this);
}

const InstrumentDescriptor &ObservableInstrument::GetInstrumentDescriptor()
{
  return instrument_descriptor_;
}

AsyncWritableMetricStorage *ObservableInstrument::GetMetricStorage()
{
  return storage_.get();
}

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
