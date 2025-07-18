// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstdint>
#include <map>
#include <mutex>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "opentelemetry/common/timestamp.h"
#include "opentelemetry/metrics/async_instruments.h"
#include "opentelemetry/metrics/observer_result.h"
#include "opentelemetry/nostd/shared_ptr.h"
#include "opentelemetry/nostd/variant.h"
#include "opentelemetry/sdk/common/global_log_handler.h"
#include "opentelemetry/sdk/metrics/async_instruments.h"
#include "opentelemetry/sdk/metrics/instruments.h"
#include "opentelemetry/sdk/metrics/observer_result.h"
#include "opentelemetry/sdk/metrics/state/metric_storage.h"
#include "opentelemetry/sdk/metrics/state/observable_registry.h"
#include "opentelemetry/sdk/metrics/view/attributes_processor.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{

void ObservableRegistry::AddCallback(opentelemetry::metrics::ObservableCallbackPtr callback,
                                     void *state,
                                     opentelemetry::metrics::ObservableInstrument *instrument)
{
  // TBD - Check if existing
  std::unique_ptr<ObservableCallbackRecord> record(
      new ObservableCallbackRecord{callback, state, instrument});
  std::lock_guard<std::mutex> lock_guard{callbacks_m_};
  callbacks_.push_back(std::move(record));
}

void ObservableRegistry::RemoveCallback(opentelemetry::metrics::ObservableCallbackPtr callback,
                                        void *state,
                                        opentelemetry::metrics::ObservableInstrument *instrument)
{
  std::lock_guard<std::mutex> lock_guard{callbacks_m_};
  auto new_end = std::remove_if(
      callbacks_.begin(), callbacks_.end(),
      [callback, state, instrument](const std::unique_ptr<ObservableCallbackRecord> &record) {
        return record->callback == callback && record->state == state &&
               record->instrument == instrument;
      });
  callbacks_.erase(new_end, callbacks_.end());
}

void ObservableRegistry::CleanupCallback(opentelemetry::metrics::ObservableInstrument *instrument)
{
  std::lock_guard<std::mutex> lock_guard{callbacks_m_};
  auto iter = std::remove_if(callbacks_.begin(), callbacks_.end(),
                             [instrument](const std::unique_ptr<ObservableCallbackRecord> &record) {
                               return record->instrument == instrument;
                             });
  callbacks_.erase(iter, callbacks_.end());
}

void ObservableRegistry::Observe(opentelemetry::common::SystemTimestamp collection_ts)
{
  static DefaultAttributesProcessor default_attribute_processor;
  std::lock_guard<std::mutex> lock_guard{callbacks_m_};
  for (auto &callback_wrap : callbacks_)
  {
    auto value_type =
        static_cast<opentelemetry::sdk::metrics::ObservableInstrument *>(callback_wrap->instrument)
            ->GetInstrumentDescriptor()
            .value_type_;
    auto storage =
        static_cast<opentelemetry::sdk::metrics::ObservableInstrument *>(callback_wrap->instrument)
            ->GetMetricStorage();
    if (!storage)
    {
      OTEL_INTERNAL_LOG_ERROR("[ObservableRegistry::Observe] - Error during observe."
                              << "The metric storage is invalid");
      return;
    }
    if (value_type == InstrumentValueType::kDouble)
    {
      nostd::shared_ptr<opentelemetry::metrics::ObserverResultT<double>> ob_res(
          new opentelemetry::sdk::metrics::ObserverResultT<double>(&default_attribute_processor));
      callback_wrap->callback(ob_res, callback_wrap->state);
      storage->RecordDouble(
          static_cast<opentelemetry::sdk::metrics::ObserverResultT<double> *>(ob_res.get())
              ->GetMeasurements(),
          collection_ts);
    }
    else
    {
      nostd::shared_ptr<opentelemetry::metrics::ObserverResultT<int64_t>> ob_res(
          new opentelemetry::sdk::metrics::ObserverResultT<int64_t>(&default_attribute_processor));
      callback_wrap->callback(ob_res, callback_wrap->state);
      storage->RecordLong(
          static_cast<opentelemetry::sdk::metrics::ObserverResultT<int64_t> *>(ob_res.get())
              ->GetMeasurements(),
          collection_ts);
    }
  }
}

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
