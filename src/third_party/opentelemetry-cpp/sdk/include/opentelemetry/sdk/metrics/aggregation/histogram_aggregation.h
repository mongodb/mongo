// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <algorithm>
#include <memory>
#include <vector>

#include "opentelemetry/common/spin_lock_mutex.h"
#include "opentelemetry/nostd/variant.h"
#include "opentelemetry/sdk/metrics/aggregation/aggregation.h"
#include "opentelemetry/sdk/metrics/aggregation/aggregation_config.h"
#include "opentelemetry/sdk/metrics/data/metric_data.h"
#include "opentelemetry/sdk/metrics/data/point_data.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{
class AggregationConfig;

class LongHistogramAggregation : public Aggregation
{
public:
  LongHistogramAggregation(const AggregationConfig *aggregation_config = nullptr);
  LongHistogramAggregation(HistogramPointData &&);
  LongHistogramAggregation(const HistogramPointData &);

  void Aggregate(int64_t value, const PointAttributes &attributes = {}) noexcept override;

  void Aggregate(double /* value */, const PointAttributes & /* attributes */) noexcept override {}

  /* Returns the result of merge of the existing aggregation with delta aggregation with same
   * boundaries */
  std::unique_ptr<Aggregation> Merge(const Aggregation &delta) const noexcept override;

  /* Returns the new delta aggregation by comparing existing aggregation with next aggregation with
   * same boundaries. Data points for `next` aggregation (sum , bucket-counts) should be more than
   * the current aggregation - which is the normal scenario as measurements values are monotonic
   * increasing.
   */
  std::unique_ptr<Aggregation> Diff(const Aggregation &next) const noexcept override;

  PointType ToPoint() const noexcept override;

private:
  mutable opentelemetry::common::SpinLockMutex lock_;
  HistogramPointData point_data_;
  bool record_min_max_ = true;
};

class DoubleHistogramAggregation : public Aggregation
{
public:
  DoubleHistogramAggregation(const AggregationConfig *aggregation_config = nullptr);
  DoubleHistogramAggregation(HistogramPointData &&);
  DoubleHistogramAggregation(const HistogramPointData &);

  void Aggregate(int64_t /* value */, const PointAttributes & /* attributes */) noexcept override {}

  void Aggregate(double value, const PointAttributes &attributes = {}) noexcept override;

  /* Returns the result of merge of the existing aggregation with delta aggregation with same
   * boundaries */
  std::unique_ptr<Aggregation> Merge(const Aggregation &delta) const noexcept override;

  /* Returns the new delta aggregation by comparing existing aggregation with next aggregation with
   * same boundaries. Data points for `next` aggregation (sum , bucket-counts) should be more than
   * the current aggregation - which is the normal scenario as measurements values are monotonic
   * increasing.
   */
  std::unique_ptr<Aggregation> Diff(const Aggregation &next) const noexcept override;

  PointType ToPoint() const noexcept override;

private:
  mutable opentelemetry::common::SpinLockMutex lock_;
  mutable HistogramPointData point_data_;
  bool record_min_max_ = true;
};

template <class T>
void HistogramMerge(HistogramPointData &current,
                    HistogramPointData &delta,
                    HistogramPointData &merge)
{
  for (size_t i = 0; i < current.counts_.size(); i++)
  {
    merge.counts_[i] = current.counts_[i] + delta.counts_[i];
  }
  merge.boundaries_     = current.boundaries_;
  merge.sum_            = nostd::get<T>(current.sum_) + nostd::get<T>(delta.sum_);
  merge.count_          = current.count_ + delta.count_;
  merge.record_min_max_ = current.record_min_max_ && delta.record_min_max_;
  if (merge.record_min_max_)
  {
    merge.min_ = (std::min)(nostd::get<T>(current.min_), nostd::get<T>(delta.min_));
    merge.max_ = (std::max)(nostd::get<T>(current.max_), nostd::get<T>(delta.max_));
  }
}

template <class T>
void HistogramDiff(HistogramPointData &current, HistogramPointData &next, HistogramPointData &diff)
{
  for (size_t i = 0; i < current.counts_.size(); i++)
  {
    diff.counts_[i] = next.counts_[i] - current.counts_[i];
  }
  diff.boundaries_     = current.boundaries_;
  diff.count_          = next.count_ - current.count_;
  diff.record_min_max_ = false;
}

template <class T>
size_t BucketBinarySearch(T value, const std::vector<double> &boundaries)
{
  auto low = std::lower_bound(boundaries.begin(), boundaries.end(), value);
  return low - boundaries.begin();
}

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
