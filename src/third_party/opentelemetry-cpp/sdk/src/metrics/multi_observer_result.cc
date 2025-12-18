// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <unordered_map>
#include <utility>

#include "opentelemetry/common/timestamp.h"
#include "opentelemetry/metrics/async_instruments.h"
#include "opentelemetry/metrics/observer_result.h"
#include "opentelemetry/nostd/function_ref.h"
#include "opentelemetry/nostd/variant.h"
#include "opentelemetry/sdk/common/global_log_handler.h"
#include "opentelemetry/sdk/metrics/async_instruments.h"
#include "opentelemetry/sdk/metrics/multi_observer_result.h"
#include "opentelemetry/sdk/metrics/observer_result.h"
#include "opentelemetry/sdk/metrics/state/metric_storage.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{

namespace
{
struct StoreResultVisitor
{
  void operator()(ObserverResultT<int64_t> &result) const
  {
    storage->RecordLong(result.GetMeasurements(), collection_ts);
  }

  void operator()(ObserverResultT<double> &result) const
  {
    storage->RecordDouble(result.GetMeasurements(), collection_ts);
  }

  void operator()(const nostd::monostate &) const {}

  AsyncWritableMetricStorage *storage;
  opentelemetry::common::SystemTimestamp collection_ts;
};
}  // namespace

void MultiObserverResult::RegisterInstrument(
    opentelemetry::metrics::ObservableInstrument *instrument)
{
  observer_results_.emplace(instrument, nostd::monostate());
}

void MultiObserverResult::DeregisterInstrument(
    opentelemetry::metrics::ObservableInstrument *instrument)
{
  observer_results_.erase(instrument);
}

size_t MultiObserverResult::InstrumentCount() const
{
  return observer_results_.size();
}

bool MultiObserverResult::HasInstrument(
    const opentelemetry::metrics::ObservableInstrument *instrument) const
{
  return observer_results_.find(const_cast<opentelemetry::metrics::ObservableInstrument *>(
             instrument)) != observer_results_.end();
}

void MultiObserverResult::GetInstruments(
    nostd::function_ref<void(opentelemetry::metrics::ObservableInstrument *)> callback)
{
  for (auto &el : observer_results_)
  {
    callback(el.first);
  }
}

void MultiObserverResult::Reset()
{
  for (auto it = observer_results_.begin(); it != observer_results_.end(); ++it)
  {
    it->second = nostd::monostate();
  }
}

void MultiObserverResult::StoreResults(opentelemetry::common::SystemTimestamp collection_ts)
{
  for (auto &el : observer_results_)
  {
    auto *instrument = el.first;
    auto &result     = el.second;

    auto storage = static_cast<opentelemetry::sdk::metrics::ObservableInstrument *>(instrument)
                       ->GetMetricStorage();
    nostd::visit(StoreResultVisitor{storage, collection_ts}, result);
  }
}

opentelemetry::metrics::ObserverResultT<double> &MultiObserverResult::ForInstrumentDouble(
    const opentelemetry::metrics::ObservableInstrument *instrument)
{
  static opentelemetry::sdk::metrics::ObserverResultT<double> null_result;
  // const_cast is appropriate here, because we're _not_ modifying the passed-in pointer;
  // we just need to make it non-const to be able to look it up in our map.
  auto it = observer_results_.find(
      const_cast<opentelemetry::metrics::ObservableInstrument *>(instrument));
  if (it == observer_results_.end())
  {
    OTEL_INTERNAL_LOG_ERROR("[MultiObserverResult::ForInstrumentDouble]"
                            << "The instrument is not registered on with callback");
    return null_result;
  }
  auto *inner = nostd::get_if<ObserverResultT<double>>(&it->second);
  if (inner == nullptr)
  {
    return it->second.emplace<ObserverResultT<double>>();
  }
  else
  {
    return *inner;
  }
}

opentelemetry::metrics::ObserverResultT<int64_t> &MultiObserverResult::ForInstrumentInt64(
    const opentelemetry::metrics::ObservableInstrument *instrument)
{
  static opentelemetry::sdk::metrics::ObserverResultT<int64_t> null_result;
  auto it = observer_results_.find(
      const_cast<opentelemetry::metrics::ObservableInstrument *>(instrument));
  if (it == observer_results_.end())
  {
    OTEL_INTERNAL_LOG_ERROR("[MultiObserverResult::ForInstrumentInt64]"
                            << "The instrument is not registered on with callback");
    return null_result;
  }

  auto *inner = nostd::get_if<ObserverResultT<int64_t>>(&it->second);
  if (inner == nullptr)
  {
    return it->second.emplace<ObserverResultT<int64_t>>();
  }
  else
  {
    return *inner;
  }
}

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
