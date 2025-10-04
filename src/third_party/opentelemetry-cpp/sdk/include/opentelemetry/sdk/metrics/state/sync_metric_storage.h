// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <functional>
#include <memory>
#include <mutex>

#include "opentelemetry/common/key_value_iterable.h"
#include "opentelemetry/common/spin_lock_mutex.h"
#include "opentelemetry/common/timestamp.h"
#include "opentelemetry/context/context.h"
#include "opentelemetry/nostd/function_ref.h"
#include "opentelemetry/nostd/span.h"
#include "opentelemetry/nostd/string_view.h"
#include "opentelemetry/sdk/common/attributemap_hash.h"
#include "opentelemetry/sdk/metrics/aggregation/aggregation.h"
#include "opentelemetry/sdk/metrics/aggregation/default_aggregation.h"
#include "opentelemetry/sdk/metrics/aggregation/histogram_aggregation.h"
#include "opentelemetry/sdk/metrics/data/metric_data.h"
#include "opentelemetry/sdk/metrics/instruments.h"
#include "opentelemetry/sdk/metrics/state/attributes_hashmap.h"
#include "opentelemetry/sdk/metrics/state/metric_collector.h"
#include "opentelemetry/sdk/metrics/state/metric_storage.h"
#include "opentelemetry/sdk/metrics/state/temporal_metric_storage.h"
#include "opentelemetry/sdk/metrics/view/attributes_processor.h"
#include "opentelemetry/version.h"

#ifdef ENABLE_METRICS_EXEMPLAR_PREVIEW
#  include "opentelemetry/sdk/metrics/exemplar/filter_type.h"
#  include "opentelemetry/sdk/metrics/exemplar/reservoir.h"
#endif

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{
class SyncMetricStorage : public MetricStorage, public SyncWritableMetricStorage
{

#ifdef ENABLE_METRICS_EXEMPLAR_PREVIEW

  static inline bool EnableExamplarFilter(ExemplarFilterType filter_type,
                                          const opentelemetry::context::Context &context)
  {
    return filter_type == ExemplarFilterType::kAlwaysOn ||
           (filter_type == ExemplarFilterType::kTraceBased &&
            opentelemetry::trace::GetSpan(context)->GetContext().IsValid() &&
            opentelemetry::trace::GetSpan(context)->GetContext().IsSampled());
  }

#endif  // ENABLE_METRICS_EXEMPLAR_PREVIEW

public:
  SyncMetricStorage(InstrumentDescriptor instrument_descriptor,
                    const AggregationType aggregation_type,
                    const AttributesProcessor *attributes_processor,
#ifdef ENABLE_METRICS_EXEMPLAR_PREVIEW
                    ExemplarFilterType exempler_filter_type,
                    nostd::shared_ptr<ExemplarReservoir> &&exemplar_reservoir,
#endif
                    const AggregationConfig *aggregation_config,
                    size_t attributes_limit = kAggregationCardinalityLimit)
      : instrument_descriptor_(instrument_descriptor),
        attributes_hashmap_(new AttributesHashMap(attributes_limit)),
        attributes_processor_(attributes_processor),
#ifdef ENABLE_METRICS_EXEMPLAR_PREVIEW
        exemplar_filter_type_(exempler_filter_type),
        exemplar_reservoir_(exemplar_reservoir),
#endif
        temporal_metric_storage_(instrument_descriptor, aggregation_type, aggregation_config)
  {
    create_default_aggregation_ = [&, aggregation_type,
                                   aggregation_config]() -> std::unique_ptr<Aggregation> {
      return DefaultAggregation::CreateAggregation(aggregation_type, instrument_descriptor_,
                                                   aggregation_config);
    };
  }

  void RecordLong(int64_t value,
                  const opentelemetry::context::Context &context
                      OPENTELEMETRY_MAYBE_UNUSED) noexcept override
  {
    if (instrument_descriptor_.value_type_ != InstrumentValueType::kLong)
    {
      return;
    }
#ifdef ENABLE_METRICS_EXEMPLAR_PREVIEW
    if (EnableExamplarFilter(exemplar_filter_type_, context))
    {
      exemplar_reservoir_->OfferMeasurement(value, {}, context, std::chrono::system_clock::now());
    }
#endif
    static size_t hash = opentelemetry::sdk::common::GetHash("");
    std::lock_guard<opentelemetry::common::SpinLockMutex> guard(attribute_hashmap_lock_);
    attributes_hashmap_->GetOrSetDefault(create_default_aggregation_, hash)->Aggregate(value);
  }

  void RecordLong(int64_t value,
                  const opentelemetry::common::KeyValueIterable &attributes,
                  const opentelemetry::context::Context &context
                      OPENTELEMETRY_MAYBE_UNUSED) noexcept override
  {
    if (instrument_descriptor_.value_type_ != InstrumentValueType::kLong)
    {
      return;
    }
#ifdef ENABLE_METRICS_EXEMPLAR_PREVIEW
    if (EnableExamplarFilter(exemplar_filter_type_, context))
    {
      exemplar_reservoir_->OfferMeasurement(value, attributes, context,
                                            std::chrono::system_clock::now());
    }
#endif
    auto hash = opentelemetry::sdk::common::GetHashForAttributeMap(
        attributes, [this](nostd::string_view key) {
          if (attributes_processor_)
          {
            return attributes_processor_->isPresent(key);
          }
          else
          {
            return true;
          }
        });

    std::lock_guard<opentelemetry::common::SpinLockMutex> guard(attribute_hashmap_lock_);
    attributes_hashmap_
        ->GetOrSetDefault(attributes, attributes_processor_, create_default_aggregation_, hash)
        ->Aggregate(value);
  }

  void RecordDouble(double value,
                    const opentelemetry::context::Context &context
                        OPENTELEMETRY_MAYBE_UNUSED) noexcept override
  {
    if (instrument_descriptor_.value_type_ != InstrumentValueType::kDouble)
    {
      return;
    }
#ifdef ENABLE_METRICS_EXEMPLAR_PREVIEW
    if (EnableExamplarFilter(exemplar_filter_type_, context))
    {
      exemplar_reservoir_->OfferMeasurement(value, {}, context, std::chrono::system_clock::now());
    }
#endif
    static size_t hash = opentelemetry::sdk::common::GetHash("");
    std::lock_guard<opentelemetry::common::SpinLockMutex> guard(attribute_hashmap_lock_);
    attributes_hashmap_->GetOrSetDefault(create_default_aggregation_, hash)->Aggregate(value);
  }

  void RecordDouble(double value,
                    const opentelemetry::common::KeyValueIterable &attributes,
                    const opentelemetry::context::Context &context
                        OPENTELEMETRY_MAYBE_UNUSED) noexcept override
  {
    if (instrument_descriptor_.value_type_ != InstrumentValueType::kDouble)
    {
      return;
    }
#ifdef ENABLE_METRICS_EXEMPLAR_PREVIEW
    if (EnableExamplarFilter(exemplar_filter_type_, context))
    {
      exemplar_reservoir_->OfferMeasurement(value, attributes, context,
                                            std::chrono::system_clock::now());
    }
#endif
    auto hash = opentelemetry::sdk::common::GetHashForAttributeMap(
        attributes, [this](nostd::string_view key) {
          if (attributes_processor_)
          {
            return attributes_processor_->isPresent(key);
          }
          else
          {
            return true;
          }
        });
    std::lock_guard<opentelemetry::common::SpinLockMutex> guard(attribute_hashmap_lock_);
    attributes_hashmap_
        ->GetOrSetDefault(attributes, attributes_processor_, create_default_aggregation_, hash)
        ->Aggregate(value);
  }

  bool Collect(CollectorHandle *collector,
               nostd::span<std::shared_ptr<CollectorHandle>> collectors,
               opentelemetry::common::SystemTimestamp sdk_start_ts,
               opentelemetry::common::SystemTimestamp collection_ts,
               nostd::function_ref<bool(MetricData)> callback) noexcept override;

private:
  InstrumentDescriptor instrument_descriptor_;
  // hashmap to maintain the metrics for delta collection (i.e, collection since last Collect call)
  std::unique_ptr<AttributesHashMap> attributes_hashmap_;
  std::function<std::unique_ptr<Aggregation>()> create_default_aggregation_;
  const AttributesProcessor *attributes_processor_;
#ifdef ENABLE_METRICS_EXEMPLAR_PREVIEW
  ExemplarFilterType exemplar_filter_type_;
  nostd::shared_ptr<ExemplarReservoir> exemplar_reservoir_;
#endif
  TemporalMetricStorage temporal_metric_storage_;
  opentelemetry::common::SpinLockMutex attribute_hashmap_lock_;
};

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
