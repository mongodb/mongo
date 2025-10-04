// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>
#include <mutex>
#include <vector>

#include "opentelemetry/common/timestamp.h"
#include "opentelemetry/metrics/async_instruments.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{

struct ObservableCallbackRecord
{
  opentelemetry::metrics::ObservableCallbackPtr callback;
  void *state;
  opentelemetry::metrics::ObservableInstrument *instrument;
};

class ObservableRegistry
{
public:
  void AddCallback(opentelemetry::metrics::ObservableCallbackPtr callback,
                   void *state,
                   opentelemetry::metrics::ObservableInstrument *instrument);

  void RemoveCallback(opentelemetry::metrics::ObservableCallbackPtr callback,
                      void *state,
                      opentelemetry::metrics::ObservableInstrument *instrument);

  void CleanupCallback(opentelemetry::metrics::ObservableInstrument *instrument);

  void Observe(opentelemetry::common::SystemTimestamp collection_ts);

private:
  std::vector<std::unique_ptr<ObservableCallbackRecord>> callbacks_;
  std::mutex callbacks_m_;
};

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
