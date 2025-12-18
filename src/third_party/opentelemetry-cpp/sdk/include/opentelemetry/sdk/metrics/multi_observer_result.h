// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <unordered_map>

#include "opentelemetry/metrics/async_instruments.h"
#include "opentelemetry/metrics/multi_observer_result.h"
#include "opentelemetry/metrics/observer_result.h"
#include "opentelemetry/nostd/function_ref.h"
#include "opentelemetry/nostd/variant.h"
#include "opentelemetry/sdk/metrics/observer_result.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{
class OPENTELEMETRY_EXPORT MultiObserverResult final
    : public opentelemetry::metrics::MultiObserverResult
{
public:
  void RegisterInstrument(opentelemetry::metrics::ObservableInstrument *instrument);
  void DeregisterInstrument(opentelemetry::metrics::ObservableInstrument *instrument);
  size_t InstrumentCount() const;
  bool HasInstrument(const opentelemetry::metrics::ObservableInstrument *instrument) const;
  void GetInstruments(
      nostd::function_ref<void(opentelemetry::metrics::ObservableInstrument *)> callback);
  void Reset();
  void StoreResults(opentelemetry::common::SystemTimestamp collection_ts);

protected:
  opentelemetry::metrics::ObserverResultT<double> &ForInstrumentDouble(
      const opentelemetry::metrics::ObservableInstrument *instrument) override;
  opentelemetry::metrics::ObserverResultT<int64_t> &ForInstrumentInt64(
      const opentelemetry::metrics::ObservableInstrument *instrument) override;

private:
  // This is _different_ to opentelemetry::metrics::ObserverResult because this variant is
  // a variant directly of ObserverResultT, not of _pointers_ to ObserverResultT.
  // This allows us to avoid an unnescessary layer of inderection and a bunch of allocations.
  using ObserverResultDirect =
      nostd::variant<nostd::monostate, ObserverResultT<double>, ObserverResultT<int64_t>>;
  std::unordered_map<opentelemetry::metrics::ObservableInstrument *, ObserverResultDirect>
      observer_results_;
};
}  // namespace metrics
}  // namespace sdk

OPENTELEMETRY_END_NAMESPACE
