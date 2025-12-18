// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <utility>

#include "opentelemetry/common/key_value_iterable.h"
#include "opentelemetry/common/timestamp.h"
#include "opentelemetry/metrics/async_instruments.h"
#include "opentelemetry/metrics/meter.h"
#include "opentelemetry/metrics/observer_result.h"
#include "opentelemetry/nostd/function_ref.h"
#include "opentelemetry/nostd/shared_ptr.h"
#include "opentelemetry/nostd/span.h"
#include "opentelemetry/nostd/variant.h"
#include "opentelemetry/sdk/metrics/async_instruments.h"
#include "opentelemetry/sdk/metrics/instruments.h"
#include "opentelemetry/sdk/metrics/multi_observer_result.h"
#include "opentelemetry/sdk/metrics/state/observable_registry.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{
struct ObservableCallbackRecord
{
  ObservableCallbackRecord(opentelemetry::metrics::ObservableCallbackPtr callback,
                           void *state,
                           opentelemetry::metrics::ObservableInstrument *instrument)
      : callback(callback), state(state)
  {
    observable_result.RegisterInstrument(static_cast<ObservableInstrument *>(instrument));
  }

  ObservableCallbackRecord(opentelemetry::metrics::MultiObservableCallbackPtr callback,
                           void *state,
                           nostd::span<opentelemetry::metrics::ObservableInstrument *> instruments)
      : callback(callback), state(state)
  {
    for (auto *instrument : instruments)
    {
      observable_result.RegisterInstrument(static_cast<ObservableInstrument *>(instrument));
    }
  }

  nostd::variant<opentelemetry::metrics::ObservableCallbackPtr,
                 opentelemetry::metrics::MultiObservableCallbackPtr>
      callback;
  void *state;
  MultiObserverResult observable_result;
};

ObservableRegistry::ObservableRegistry()  = default;
ObservableRegistry::~ObservableRegistry() = default;

void ObservableRegistry::AddCallback(opentelemetry::metrics::ObservableCallbackPtr callback,
                                     void *state,
                                     opentelemetry::metrics::ObservableInstrument *instrument)
{
  auto record = std::make_unique<ObservableCallbackRecord>(callback, state, instrument);
  auto token  = reinterpret_cast<uintptr_t>(record.get());

  std::lock_guard<std::mutex> lock_guard{callbacks_m_};
  callbacks_.insert({token, std::move(record)});
}

uintptr_t ObservableRegistry::AddCallback(
    opentelemetry::metrics::MultiObservableCallbackPtr callback,
    void *state,
    nostd::span<opentelemetry::metrics::ObservableInstrument *> instruments)
{
  auto record = std::make_unique<ObservableCallbackRecord>(callback, state, instruments);
  auto token  = reinterpret_cast<uintptr_t>(record.get());

  std::lock_guard<std::mutex> lock_guard{callbacks_m_};
  callbacks_.insert({token, std::move(record)});
  return token;
}

void ObservableRegistry::RemoveCallback(opentelemetry::metrics::ObservableCallbackPtr callback,
                                        void *state,
                                        opentelemetry::metrics::ObservableInstrument *instrument)
{
  std::lock_guard<std::mutex> lock_guard{callbacks_m_};
  for (auto it = callbacks_.begin(); it != callbacks_.end();)
  {
    const auto &record = it->second;
    // Remove the callback if it's registered with the the single-instrument signature
    auto observable_callback_ptr =
        nostd::get_if<opentelemetry::metrics::ObservableCallbackPtr>(&record->callback);
    if (observable_callback_ptr && *observable_callback_ptr == callback && record->state == state &&
        record->observable_result.HasInstrument(instrument))
    {
      it = callbacks_.erase(it);
    }
    else
    {
      ++it;
    }
  }
}

void ObservableRegistry::RemoveCallback(uintptr_t id)
{
  std::lock_guard<std::mutex> lock_guard{callbacks_m_};
  callbacks_.erase(id);
}

void ObservableRegistry::CleanupCallback(opentelemetry::metrics::ObservableInstrument *instrument)
{
  std::lock_guard<std::mutex> lock_guard{callbacks_m_};
  auto sdk_instrument = static_cast<ObservableInstrument *>(instrument);
  for (auto it = callbacks_.begin(); it != callbacks_.end();)
  {
    // Remove the instrument from the multi-callback when the instrument is destroyed
    it->second->observable_result.DeregisterInstrument(sdk_instrument);

    // If the multi-callback has no instruments left, remove it from the registry
    if (it->second->observable_result.InstrumentCount() == 0)
    {
      it = callbacks_.erase(it);
    }
    else
    {
      ++it;
    }
  }
}

namespace
{

template <typename T>
class ObserverResultTAdapter : public opentelemetry::metrics::ObserverResultT<T>
{
public:
  ObserverResultTAdapter(opentelemetry::metrics::ObserverResultT<T> *inner) : inner(inner) {}

  void Observe(T value) noexcept override
  {
    if (inner)
    {
      inner->Observe(value);
    }
  }
  void Observe(T value, const opentelemetry::common::KeyValueIterable &attributes) noexcept override
  {
    if (inner)
    {
      inner->Observe(value, attributes);
    }
  }
  opentelemetry::metrics::ObserverResultT<T> *inner;
};

struct InvokeCallbackVisitor
{
  void operator()(const opentelemetry::metrics::ObservableCallbackPtr &callback)
  {
    record->observable_result.GetInstruments(
        [&](opentelemetry::metrics::ObservableInstrument *instrument) {
          auto value_type =
              static_cast<opentelemetry::sdk::metrics::ObservableInstrument *>(instrument)
                  ->GetInstrumentDescriptor()
                  .value_type_;
          if (value_type == InstrumentValueType::kDouble)
          {
            invoke_single_instrument_callback<double>(callback, instrument);
          }
          else
          {
            invoke_single_instrument_callback<int64_t>(callback, instrument);
          }
        });
  }
  void operator()(const opentelemetry::metrics::MultiObservableCallbackPtr &callback)
  {
    callback(record->observable_result, record->state);
  }

  template <typename T>
  void invoke_single_instrument_callback(
      const opentelemetry::metrics::ObservableCallbackPtr &callback,
      opentelemetry::metrics::ObservableInstrument *instrument)
  {
    // This is all a bit strangely shaped, but it's in the name of back-compat.
    // The signature of ObservableCallbackPtr is that it takes a nostd::shared_ptr to
    // an abstract ObserverResultT. The new implementation of ObservableRegistry doesn't
    // actually need to box the ObserverResultT's into a shared_ptr, but we need to construct
    // such a shared_ptr here in order to invoke the callback with the old signature.
    // MultiObserverResult::ForInstrument returns a reference to an abstract ObserverResultT,
    // which is allocated inside record->observable_result; thus, we need to allocate a wrapper
    // around that which can be on the heap in the shared_ptr.
    ObserverResultTAdapter<T> *adapter =
        new ObserverResultTAdapter<T>(&record->observable_result.ForInstrument<T>(instrument));
    opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObserverResultT<T>> result_wrapper(
        adapter);
    callback(result_wrapper, record->state);
    // It's possible that callback() holds the shared_ptr after we return here. The reference to
    // the actual ObserverResultT inside record->observable_result might not be valid for the full
    // lifetime of that shared_ptr. So, null it out.
    // (yes, that means the shared_ptr will now no longer _do_ anything, but it didn't in the old
    // implementation either; the only requirement here is using the shared_ptr doesn't crash).
    adapter->inner = nullptr;
  }

  ObservableCallbackRecord *record;
};
}  // namespace

void ObservableRegistry::Observe(opentelemetry::common::SystemTimestamp collection_ts)
{
  std::lock_guard<std::mutex> lock_guard{callbacks_m_};
  for (const auto &pair : callbacks_)
  {
    auto &cb_record = pair.second;
    cb_record->observable_result.Reset();
    // Visitor will either invoke the single-instrument or multi-instrument form of the callback
    nostd::visit(InvokeCallbackVisitor{cb_record.get()}, cb_record->callback);
    cb_record->observable_result.StoreResults(collection_ts);
  }
}

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
