// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <list>
#include <memory>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "opentelemetry/common/spin_lock_mutex.h"
#include "opentelemetry/common/timestamp.h"
#include "opentelemetry/nostd/function_ref.h"
#include "opentelemetry/nostd/span.h"
#include "opentelemetry/sdk/common/attributemap_hash.h"
#include "opentelemetry/sdk/metrics/aggregation/aggregation.h"
#include "opentelemetry/sdk/metrics/aggregation/aggregation_config.h"
#include "opentelemetry/sdk/metrics/aggregation/default_aggregation.h"
#include "opentelemetry/sdk/metrics/data/metric_data.h"
#include "opentelemetry/sdk/metrics/instruments.h"
#include "opentelemetry/sdk/metrics/state/attributes_hashmap.h"
#include "opentelemetry/sdk/metrics/state/metric_collector.h"
#include "opentelemetry/sdk/metrics/state/temporal_metric_storage.h"
#include "opentelemetry/sdk/metrics/view/attributes_processor.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{

TemporalMetricStorage::TemporalMetricStorage(InstrumentDescriptor instrument_descriptor,
                                             AggregationType aggregation_type,
                                             const AggregationConfig *aggregation_config)
    : instrument_descriptor_(std::move(instrument_descriptor)),
      aggregation_type_(aggregation_type),
      aggregation_config_(aggregation_config)
{}

bool TemporalMetricStorage::buildMetrics(CollectorHandle *collector,
                                         nostd::span<std::shared_ptr<CollectorHandle>> collectors,
                                         opentelemetry::common::SystemTimestamp sdk_start_ts,
                                         opentelemetry::common::SystemTimestamp collection_ts,
                                         const std::shared_ptr<AttributesHashMap> &delta_metrics,
                                         nostd::function_ref<bool(MetricData)> callback) noexcept
{
  std::lock_guard<opentelemetry::common::SpinLockMutex> guard(lock_);
  opentelemetry::common::SystemTimestamp last_collection_ts = sdk_start_ts;
  AggregationTemporality aggregation_temporarily =
      collector->GetAggregationTemporality(instrument_descriptor_.type_);

  if (delta_metrics->Size())
  {
    for (auto &col : collectors)
    {
      unreported_metrics_[col.get()].push_back(delta_metrics);
    }
  }

  // Get the unreported metrics for the `collector` from `unreported metrics stash`
  // since last collection, this will also cleanup the unreported metrics for `collector`
  // from the stash.
  auto present = unreported_metrics_.find(collector);
  if (present == unreported_metrics_.end())
  {
    // no unreported metrics for the collector, return.
    return true;
  }
  auto unreported_list = std::move(present->second);
  // Iterate over the unreporter metrics for `collector` and store result in `merged_metrics`
  std::unique_ptr<AttributesHashMap> merged_metrics(new AttributesHashMap);
  for (auto &agg_hashmap : unreported_list)
  {
    agg_hashmap->GetAllEnteries(
        [&merged_metrics, this](const MetricAttributes &attributes, Aggregation &aggregation) {
          auto hash = opentelemetry::sdk::common::GetHashForAttributeMap(attributes);
          auto agg  = merged_metrics->Get(hash);
          if (agg)
          {
            merged_metrics->Set(attributes, agg->Merge(aggregation), hash);
          }
          else
          {
            merged_metrics->Set(attributes,
                                DefaultAggregation::CreateAggregation(
                                    aggregation_type_, instrument_descriptor_, aggregation_config_)
                                    ->Merge(aggregation),
                                hash);
          }
          return true;
        });
  }
  // Get the last reported metrics for the `collector` from `last reported metrics` stash
  //   - If the aggregation_temporarily for the collector is cumulative
  //       - Merge the last reported metrics with unreported metrics (which is in merged_metrics),
  //           Final result of merge would be in merged_metrics.
  //       - Move the final merge to the `last reported metrics` stash.
  //   - If the aggregation_temporarily is delta
  //       - Store the unreported metrics for `collector` (which is in merged_mtrics) to
  //          `last reported metrics` stash.

  auto reported = last_reported_metrics_.find(collector);
  if (reported != last_reported_metrics_.end())
  {
    auto last_aggr_hashmap = std::move(last_reported_metrics_[collector].attributes_map);
    if (aggregation_temporarily == AggregationTemporality::kCumulative)
    {
      // merge current delta to previous cumulative
      last_aggr_hashmap->GetAllEnteries(
          [&merged_metrics, this](const MetricAttributes &attributes, Aggregation &aggregation) {
            auto hash = opentelemetry::sdk::common::GetHashForAttributeMap(attributes);
            auto agg  = merged_metrics->Get(hash);
            if (agg)
            {
              merged_metrics->Set(attributes, agg->Merge(aggregation), hash);
            }
            else
            {
              auto def_agg = DefaultAggregation::CreateAggregation(
                  aggregation_type_, instrument_descriptor_, aggregation_config_);
              merged_metrics->Set(attributes, def_agg->Merge(aggregation), hash);
            }
            return true;
          });
    }
    else
    {
      last_collection_ts = last_reported_metrics_[collector].collection_ts;
    }
    last_reported_metrics_[collector] =
        LastReportedMetrics{std::move(merged_metrics), collection_ts};
  }
  else
  {
    last_reported_metrics_.insert(
        std::make_pair(collector, LastReportedMetrics{std::move(merged_metrics), collection_ts}));
  }

  // Generate the MetricData from the final merged_metrics, and invoke callback over it.

  AttributesHashMap *result_to_export = (last_reported_metrics_[collector]).attributes_map.get();
  MetricData metric_data;
  metric_data.instrument_descriptor   = instrument_descriptor_;
  metric_data.aggregation_temporality = aggregation_temporarily;
  metric_data.start_ts                = last_collection_ts;
  metric_data.end_ts                  = collection_ts;
  result_to_export->GetAllEnteries(
      [&metric_data](const MetricAttributes &attributes, Aggregation &aggregation) {
        PointDataAttributes point_data_attr;
        point_data_attr.point_data = aggregation.ToPoint();
        point_data_attr.attributes = attributes;
        metric_data.point_data_attr_.emplace_back(std::move(point_data_attr));
        return true;
      });
  return callback(metric_data);
}

}  // namespace metrics

}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
