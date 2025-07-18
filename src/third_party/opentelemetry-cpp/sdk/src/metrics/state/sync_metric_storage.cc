// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <memory>
#include <mutex>
#include <utility>

#include "opentelemetry/common/spin_lock_mutex.h"
#include "opentelemetry/common/timestamp.h"
#include "opentelemetry/nostd/function_ref.h"
#include "opentelemetry/nostd/span.h"
#include "opentelemetry/sdk/metrics/data/metric_data.h"
#include "opentelemetry/sdk/metrics/state/attributes_hashmap.h"
#include "opentelemetry/sdk/metrics/state/metric_collector.h"
#include "opentelemetry/sdk/metrics/state/sync_metric_storage.h"
#include "opentelemetry/sdk/metrics/state/temporal_metric_storage.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{

bool SyncMetricStorage::Collect(CollectorHandle *collector,
                                nostd::span<std::shared_ptr<CollectorHandle>> collectors,
                                opentelemetry::common::SystemTimestamp sdk_start_ts,
                                opentelemetry::common::SystemTimestamp collection_ts,
                                nostd::function_ref<bool(MetricData)> callback) noexcept
{
  // Add the current delta metrics to `unreported metrics stash` for all the collectors,
  // this will also empty the delta metrics hashmap, and make it available for
  // recordings
  std::shared_ptr<AttributesHashMap> delta_metrics = nullptr;
  {
    std::lock_guard<opentelemetry::common::SpinLockMutex> guard(attribute_hashmap_lock_);
    delta_metrics = std::move(attributes_hashmap_);
    attributes_hashmap_.reset(new AttributesHashMap);
  }

  return temporal_metric_storage_.buildMetrics(collector, collectors, sdk_start_ts, collection_ts,
                                               delta_metrics, callback);
}

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
