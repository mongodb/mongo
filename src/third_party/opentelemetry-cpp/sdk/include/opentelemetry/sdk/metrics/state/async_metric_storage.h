// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>
#include <mutex>
#include <unordered_map>

#include "opentelemetry/nostd/shared_ptr.h"
#include "opentelemetry/sdk/common/attributemap_hash.h"
#include "opentelemetry/sdk/metrics/aggregation/default_aggregation.h"

#ifdef ENABLE_METRICS_EXEMPLAR_PREVIEW
#  include "opentelemetry/sdk/metrics/exemplar/filter_type.h"
#  include "opentelemetry/sdk/metrics/exemplar/reservoir.h"
#endif

#include "opentelemetry/sdk/metrics/instruments.h"
#include "opentelemetry/sdk/metrics/observer_result.h"
#include "opentelemetry/sdk/metrics/state/attributes_hashmap.h"
#include "opentelemetry/sdk/metrics/state/metric_collector.h"
#include "opentelemetry/sdk/metrics/state/metric_storage.h"
#include "opentelemetry/sdk/metrics/state/temporal_metric_storage.h"
#include "opentelemetry/sdk/metrics/view/attributes_processor.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{

class AsyncMetricStorage : public MetricStorage, public AsyncWritableMetricStorage
{
public:
  AsyncMetricStorage(InstrumentDescriptor instrument_descriptor,
                     const AggregationType aggregation_type,
#ifdef ENABLE_METRICS_EXEMPLAR_PREVIEW
                     ExemplarFilterType exempler_filter_type,
                     nostd::shared_ptr<ExemplarReservoir> &&exemplar_reservoir,
#endif
                     const AggregationConfig *aggregation_config)
      : instrument_descriptor_(instrument_descriptor),
        aggregation_type_{aggregation_type},
        cumulative_hash_map_(new AttributesHashMap()),
        delta_hash_map_(new AttributesHashMap()),
#ifdef ENABLE_METRICS_EXEMPLAR_PREVIEW
        exemplar_filter_type_(exempler_filter_type),
        exemplar_reservoir_(exemplar_reservoir),
#endif
        temporal_metric_storage_(instrument_descriptor, aggregation_type, aggregation_config)
  {}

  template <class T>
  void Record(const std::unordered_map<MetricAttributes, T, AttributeHashGenerator> &measurements,
              opentelemetry::common::SystemTimestamp /* observation_time */) noexcept
  {
    // Async counter always record monotonically increasing values, and the
    // exporter/reader can request either for delta or cumulative value.
    // So we convert the async counter value to delta before passing it to temporal storage.
    std::lock_guard<opentelemetry::common::SpinLockMutex> guard(hashmap_lock_);
    for (auto &measurement : measurements)
    {
#ifdef ENABLE_METRICS_EXEMPLAR_PREVIEW
      if (exemplar_filter_type_ == ExemplarFilterType::kAlwaysOn)
      {
        exemplar_reservoir_->OfferMeasurement(measurement.second, {}, {},
                                              std::chrono::system_clock::now());
      }
#endif

      auto aggr = DefaultAggregation::CreateAggregation(aggregation_type_, instrument_descriptor_);
      aggr->Aggregate(measurement.second);
      auto hash = opentelemetry::sdk::common::GetHashForAttributeMap(measurement.first);
      auto prev = cumulative_hash_map_->Get(hash);
      if (prev)
      {
        auto delta = prev->Diff(*aggr);
        // store received value in cumulative map, and the diff in delta map (to pass it to temporal
        // storage)
        cumulative_hash_map_->Set(measurement.first, std::move(aggr), hash);
        delta_hash_map_->Set(measurement.first, std::move(delta), hash);
      }
      else
      {
        // store received value in cumulative and delta map.
        cumulative_hash_map_->Set(
            measurement.first,
            DefaultAggregation::CloneAggregation(aggregation_type_, instrument_descriptor_, *aggr),
            hash);
        delta_hash_map_->Set(measurement.first, std::move(aggr), hash);
      }
    }
  }

  void RecordLong(
      const std::unordered_map<MetricAttributes, int64_t, AttributeHashGenerator> &measurements,
      opentelemetry::common::SystemTimestamp observation_time) noexcept override
  {
    if (instrument_descriptor_.value_type_ != InstrumentValueType::kLong)
    {
      return;
    }
    Record<int64_t>(measurements, observation_time);
  }

  void RecordDouble(
      const std::unordered_map<MetricAttributes, double, AttributeHashGenerator> &measurements,
      opentelemetry::common::SystemTimestamp observation_time) noexcept override
  {
    if (instrument_descriptor_.value_type_ != InstrumentValueType::kDouble)
    {
      return;
    }
    Record<double>(measurements, observation_time);
  }

  bool Collect(CollectorHandle *collector,
               nostd::span<std::shared_ptr<CollectorHandle>> collectors,
               opentelemetry::common::SystemTimestamp sdk_start_ts,
               opentelemetry::common::SystemTimestamp collection_ts,
               nostd::function_ref<bool(MetricData)> metric_collection_callback) noexcept override
  {

    std::shared_ptr<AttributesHashMap> delta_metrics = nullptr;
    {
      std::lock_guard<opentelemetry::common::SpinLockMutex> guard(hashmap_lock_);
      delta_metrics = std::move(delta_hash_map_);
      delta_hash_map_.reset(new AttributesHashMap);
    }

    auto status =
        temporal_metric_storage_.buildMetrics(collector, collectors, sdk_start_ts, collection_ts,
                                              delta_metrics, metric_collection_callback);
    return status;
  }

private:
  InstrumentDescriptor instrument_descriptor_;
  AggregationType aggregation_type_;
  std::unique_ptr<AttributesHashMap> cumulative_hash_map_;
  std::unique_ptr<AttributesHashMap> delta_hash_map_;
  opentelemetry::common::SpinLockMutex hashmap_lock_;
#ifdef ENABLE_METRICS_EXEMPLAR_PREVIEW
  ExemplarFilterType exemplar_filter_type_;
  nostd::shared_ptr<ExemplarReservoir> exemplar_reservoir_;
#endif
  TemporalMetricStorage temporal_metric_storage_;
};

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
