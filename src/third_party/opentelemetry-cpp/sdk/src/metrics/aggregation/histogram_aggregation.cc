// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <stddef.h>
#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "opentelemetry/common/spin_lock_mutex.h"
#include "opentelemetry/nostd/variant.h"
#include "opentelemetry/sdk/metrics/aggregation/aggregation.h"
#include "opentelemetry/sdk/metrics/aggregation/aggregation_config.h"
#include "opentelemetry/sdk/metrics/aggregation/histogram_aggregation.h"
#include "opentelemetry/sdk/metrics/data/metric_data.h"
#include "opentelemetry/sdk/metrics/data/point_data.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{

LongHistogramAggregation::LongHistogramAggregation(const AggregationConfig *aggregation_config)
{
  auto ac = static_cast<const HistogramAggregationConfig *>(aggregation_config);
  if (ac)
  {
    point_data_.boundaries_ = ac->boundaries_;
  }
  else
  {
    point_data_.boundaries_ = {0.0,   5.0,   10.0,   25.0,   50.0,   75.0,   100.0,  250.0,
                               500.0, 750.0, 1000.0, 2500.0, 5000.0, 7500.0, 10000.0};
  }

  if (ac)
  {
    record_min_max_ = ac->record_min_max_;
  }
  point_data_.counts_         = std::vector<uint64_t>(point_data_.boundaries_.size() + 1, 0);
  point_data_.sum_            = static_cast<int64_t>(0);
  point_data_.count_          = 0;
  point_data_.record_min_max_ = record_min_max_;
  point_data_.min_            = (std::numeric_limits<int64_t>::max)();
  point_data_.max_            = (std::numeric_limits<int64_t>::min)();
}

LongHistogramAggregation::LongHistogramAggregation(HistogramPointData &&data)
    : point_data_{std::move(data)}, record_min_max_{point_data_.record_min_max_}
{}

LongHistogramAggregation::LongHistogramAggregation(const HistogramPointData &data)
    : point_data_{data}, record_min_max_{point_data_.record_min_max_}
{}

void LongHistogramAggregation::Aggregate(int64_t value,
                                         const PointAttributes & /* attributes */) noexcept
{
  const std::lock_guard<opentelemetry::common::SpinLockMutex> locked(lock_);
  point_data_.count_ += 1;
  point_data_.sum_ = nostd::get<int64_t>(point_data_.sum_) + value;
  if (record_min_max_)
  {
    point_data_.min_ = (std::min)(nostd::get<int64_t>(point_data_.min_), value);
    point_data_.max_ = (std::max)(nostd::get<int64_t>(point_data_.max_), value);
  }
  size_t index = BucketBinarySearch(value, point_data_.boundaries_);
  point_data_.counts_[index] += 1;
}

std::unique_ptr<Aggregation> LongHistogramAggregation::Merge(
    const Aggregation &delta) const noexcept
{
  auto curr_value  = nostd::get<HistogramPointData>(ToPoint());
  auto delta_value = nostd::get<HistogramPointData>(
      (static_cast<const LongHistogramAggregation &>(delta).ToPoint()));
  HistogramAggregationConfig agg_config;
  agg_config.boundaries_         = curr_value.boundaries_;
  agg_config.record_min_max_     = record_min_max_;
  LongHistogramAggregation *aggr = new LongHistogramAggregation(&agg_config);
  HistogramMerge<int64_t>(curr_value, delta_value, aggr->point_data_);
  return std::unique_ptr<Aggregation>(aggr);
}

std::unique_ptr<Aggregation> LongHistogramAggregation::Diff(const Aggregation &next) const noexcept
{
  auto curr_value = nostd::get<HistogramPointData>(ToPoint());
  auto next_value = nostd::get<HistogramPointData>(
      (static_cast<const LongHistogramAggregation &>(next).ToPoint()));
  HistogramAggregationConfig agg_config;
  agg_config.boundaries_         = curr_value.boundaries_;
  agg_config.record_min_max_     = record_min_max_;
  LongHistogramAggregation *aggr = new LongHistogramAggregation(&agg_config);
  HistogramDiff<int64_t>(curr_value, next_value, aggr->point_data_);
  return std::unique_ptr<Aggregation>(aggr);
}

PointType LongHistogramAggregation::ToPoint() const noexcept
{
  const std::lock_guard<opentelemetry::common::SpinLockMutex> locked(lock_);
  return point_data_;
}

DoubleHistogramAggregation::DoubleHistogramAggregation(const AggregationConfig *aggregation_config)
{
  auto ac = static_cast<const HistogramAggregationConfig *>(aggregation_config);
  if (ac)
  {
    point_data_.boundaries_ = ac->boundaries_;
  }
  else
  {
    point_data_.boundaries_ = {0.0,   5.0,   10.0,   25.0,   50.0,   75.0,   100.0,  250.0,
                               500.0, 750.0, 1000.0, 2500.0, 5000.0, 7500.0, 10000.0};
  }
  if (ac)
  {
    record_min_max_ = ac->record_min_max_;
  }
  point_data_.counts_         = std::vector<uint64_t>(point_data_.boundaries_.size() + 1, 0);
  point_data_.sum_            = 0.0;
  point_data_.count_          = 0;
  point_data_.record_min_max_ = record_min_max_;
  point_data_.min_            = (std::numeric_limits<double>::max)();
  point_data_.max_            = (std::numeric_limits<double>::min)();
}

DoubleHistogramAggregation::DoubleHistogramAggregation(HistogramPointData &&data)
    : point_data_{std::move(data)}
{}

DoubleHistogramAggregation::DoubleHistogramAggregation(const HistogramPointData &data)
    : point_data_{data}
{}

void DoubleHistogramAggregation::Aggregate(double value,
                                           const PointAttributes & /* attributes */) noexcept
{
  const std::lock_guard<opentelemetry::common::SpinLockMutex> locked(lock_);
  point_data_.count_ += 1;
  point_data_.sum_ = nostd::get<double>(point_data_.sum_) + value;
  if (record_min_max_)
  {
    point_data_.min_ = (std::min)(nostd::get<double>(point_data_.min_), value);
    point_data_.max_ = (std::max)(nostd::get<double>(point_data_.max_), value);
  }
  size_t index = BucketBinarySearch(value, point_data_.boundaries_);
  point_data_.counts_[index] += 1;
}

std::unique_ptr<Aggregation> DoubleHistogramAggregation::Merge(
    const Aggregation &delta) const noexcept
{
  auto curr_value  = nostd::get<HistogramPointData>(ToPoint());
  auto delta_value = nostd::get<HistogramPointData>(
      (static_cast<const DoubleHistogramAggregation &>(delta).ToPoint()));
  HistogramAggregationConfig agg_config;
  agg_config.boundaries_           = curr_value.boundaries_;
  agg_config.record_min_max_       = record_min_max_;
  DoubleHistogramAggregation *aggr = new DoubleHistogramAggregation(&agg_config);
  HistogramMerge<double>(curr_value, delta_value, aggr->point_data_);
  return std::unique_ptr<Aggregation>(aggr);
}

std::unique_ptr<Aggregation> DoubleHistogramAggregation::Diff(
    const Aggregation &next) const noexcept
{
  auto curr_value = nostd::get<HistogramPointData>(ToPoint());
  auto next_value = nostd::get<HistogramPointData>(
      (static_cast<const DoubleHistogramAggregation &>(next).ToPoint()));
  HistogramAggregationConfig agg_config;
  agg_config.boundaries_           = curr_value.boundaries_;
  agg_config.record_min_max_       = record_min_max_;
  DoubleHistogramAggregation *aggr = new DoubleHistogramAggregation(&agg_config);
  HistogramDiff<double>(curr_value, next_value, aggr->point_data_);
  return std::unique_ptr<Aggregation>(aggr);
}

PointType DoubleHistogramAggregation::ToPoint() const noexcept
{
  const std::lock_guard<opentelemetry::common::SpinLockMutex> locked(lock_);
  return point_data_;
}

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
