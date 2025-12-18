// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#ifdef ENABLE_METRICS_EXEMPLAR_PREVIEW

#  include <algorithm>

#  include "opentelemetry/common/macros.h"
#  include "opentelemetry/sdk/metrics/aggregation/aggregation_config.h"
#  include "opentelemetry/sdk/metrics/exemplar/aligned_histogram_bucket_exemplar_reservoir.h"
#  include "opentelemetry/sdk/metrics/exemplar/simple_fixed_size_exemplar_reservoir.h"
#  include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{
static inline MapAndResetCellType GetMapAndResetCellMethod(
    const InstrumentDescriptor &instrument_descriptor)
{
  if (instrument_descriptor.value_type_ == InstrumentValueType::kLong)
  {
    return &ReservoirCell::GetAndResetLong;
  }

  return &ReservoirCell::GetAndResetDouble;
}

static inline size_t GetSimpleFixedReservoirDefaultSize(const AggregationType agg_type,
                                                        const AggregationConfig *const agg_config)

{
  constexpr size_t kMaxBase2ExponentialHistogramReservoirSize = 20;

  if (agg_type == AggregationType::kBase2ExponentialHistogram)
  {
    const auto *histogram_agg_config =
        static_cast<const Base2ExponentialHistogramAggregationConfig *>(agg_config);
    return (std::min)(kMaxBase2ExponentialHistogramReservoirSize,
                      histogram_agg_config->max_buckets_);
  }

  return SimpleFixedSizeExemplarReservoir::kDefaultSimpleReservoirSize;
}

static inline nostd::shared_ptr<ExemplarReservoir> GetExemplarReservoir(
    const AggregationType agg_type,
    const AggregationConfig *agg_config,
    const InstrumentDescriptor &instrument_descriptor)
{
  if (agg_type == AggregationType::kHistogram)
  {
    const auto *histogram_agg_config = static_cast<const HistogramAggregationConfig *>(agg_config);

    //
    // Explicit bucket histogram aggregation with more than 1 bucket will use
    // AlignedHistogramBucketExemplarReservoir.
    // https://github.com/open-telemetry/opentelemetry-specification/blob/main/specification/metrics/sdk.md#exemplar-defaults
    //
    if (histogram_agg_config != nullptr && histogram_agg_config->boundaries_.size() > 1)
    {
      return nostd::shared_ptr<ExemplarReservoir>(new AlignedHistogramBucketExemplarReservoir(
          histogram_agg_config->boundaries_.size(),
          AlignedHistogramBucketExemplarReservoir::GetHistogramCellSelector(
              histogram_agg_config->boundaries_),
          GetMapAndResetCellMethod(instrument_descriptor)));
    }
  }

  return nostd::shared_ptr<ExemplarReservoir>(new SimpleFixedSizeExemplarReservoir(
      GetSimpleFixedReservoirDefaultSize(agg_type, agg_config),
      SimpleFixedSizeExemplarReservoir::GetSimpleFixedSizeCellSelector(),
      GetMapAndResetCellMethod(instrument_descriptor)));
}
}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
#endif  // ENABLE_METRICS_EXEMPLAR_PREVIEW
