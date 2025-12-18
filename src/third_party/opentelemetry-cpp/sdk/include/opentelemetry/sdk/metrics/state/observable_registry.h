// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "opentelemetry/common/timestamp.h"
#include "opentelemetry/metrics/async_instruments.h"
#include "opentelemetry/metrics/meter.h"
#include "opentelemetry/sdk/metrics/multi_observer_result.h"
#include "opentelemetry/version.h"

#if OPENTELEMETRY_ABI_VERSION_NO >= 2
#  include <unordered_map>
#  include "opentelemetry/nostd/span.h"
#endif

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{

struct ObservableCallbackRecord;

class ObservableRegistry
{
public:
  // Constructor & destructor need to be defined in the observable_registry.cc TU, rather
  // than implicitly defaulted here, so that we can have a unique_ptr to an incomplete
  // class as a member.
  ObservableRegistry();
  ~ObservableRegistry();

  // Add a callback of the single-instrument form
  void AddCallback(opentelemetry::metrics::ObservableCallbackPtr callback,
                   void *state,
                   opentelemetry::metrics::ObservableInstrument *instrument);
  // Add a callback with the multi-instrument signature
  uintptr_t AddCallback(opentelemetry::metrics::MultiObservableCallbackPtr callback,
                        void *state,
                        nostd::span<opentelemetry::metrics::ObservableInstrument *> instruments);
  // Callbacks added with Meter::RegisterCallback have can be removed by passing back the handle
  // returned
  void RemoveCallback(uintptr_t id);
  // Callbacks added with ObservableInstrument::AddCallback can be removed by passing back the
  // original (callback function, state, instrument).
  void RemoveCallback(opentelemetry::metrics::ObservableCallbackPtr callback,
                      void *state,
                      opentelemetry::metrics::ObservableInstrument *instrument);

  void CleanupCallback(opentelemetry::metrics::ObservableInstrument *instrument);

  void Observe(opentelemetry::common::SystemTimestamp collection_ts);

private:
  std::unordered_map<uintptr_t, std::unique_ptr<ObservableCallbackRecord>> callbacks_;
  std::mutex callbacks_m_;
};

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
