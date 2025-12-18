// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

#include "opentelemetry/metrics/async_instruments.h"
#include "opentelemetry/metrics/observer_result.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace metrics
{

class MultiObserverResult
{

public:
  virtual ~MultiObserverResult() = default;

  /**
   * Obtain an ObserverResultT<T> for the given instrument, that can be used to record
   * a measurement on said instrument from a multi-observer callback registered with
   * Meter::RegisterCallback. The instrument _must_ have been included in the original
   * call to Meter::RegisterCallback; any data points set on other instruments will be
   * discarded.
   *
   * @param instrument The instrument for which to obtain an ObserverResult.
   * @return An ObserverResultT<T> for the given instrument.
   */
  template <typename T>
  ObserverResultT<T> &ForInstrument(const ObservableInstrument *instrument) = delete;

protected:
  // You can't have a virtual template, and you can't overload on return type, so we need to
  // enumerate the options for the observer result type as separate methods to override.
  virtual ObserverResultT<double> &ForInstrumentDouble(const ObservableInstrument *instrument) = 0;
  virtual ObserverResultT<int64_t> &ForInstrumentInt64(const ObservableInstrument *instrument) = 0;
};

template <>
inline ObserverResultT<double> &MultiObserverResult::ForInstrument<double>(
    const ObservableInstrument *instrument)
{
  return ForInstrumentDouble(instrument);
}

template <>
inline ObserverResultT<int64_t> &MultiObserverResult::ForInstrument<int64_t>(
    const ObservableInstrument *instrument)
{
  return ForInstrumentInt64(instrument);
}

}  // namespace metrics
OPENTELEMETRY_END_NAMESPACE
