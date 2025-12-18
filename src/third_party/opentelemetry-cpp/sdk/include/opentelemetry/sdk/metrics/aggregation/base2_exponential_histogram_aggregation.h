// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <stdint.h>
#include <memory>

#include "opentelemetry/common/spin_lock_mutex.h"
#include "opentelemetry/sdk/metrics/aggregation/aggregation.h"
#include "opentelemetry/sdk/metrics/aggregation/aggregation_config.h"
#include "opentelemetry/sdk/metrics/aggregation/base2_exponential_histogram_indexer.h"
#include "opentelemetry/sdk/metrics/data/circular_buffer.h"
#include "opentelemetry/sdk/metrics/data/metric_data.h"
#include "opentelemetry/sdk/metrics/data/point_data.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{

class Base2ExponentialHistogramAggregation : public Aggregation
{
public:
  Base2ExponentialHistogramAggregation(const AggregationConfig *aggregation_config = nullptr);
  Base2ExponentialHistogramAggregation(const Base2ExponentialHistogramPointData &point_data);
  Base2ExponentialHistogramAggregation(Base2ExponentialHistogramPointData &&point_data);

  void Aggregate(int64_t value, const PointAttributes &attributes = {}) noexcept override;
  void Aggregate(double value, const PointAttributes &attributes = {}) noexcept override;

  /* Returns the result of merge of the existing aggregation with delta
   * aggregation with same boundaries */
  std::unique_ptr<Aggregation> Merge(const Aggregation &delta) const noexcept override;

  /* Returns the new delta aggregation by comparing existing aggregation with
   * next aggregation with same boundaries. Data points for `next` aggregation
   * (sum , bucket-counts) should be more than the current aggregation - which
   * is the normal scenario as measurements values are monotonic increasing.
   */
  std::unique_ptr<Aggregation> Diff(const Aggregation &next) const noexcept override;

  PointType ToPoint() const noexcept override;

private:
  void AggregateIntoBuckets(std::unique_ptr<AdaptingCircularBufferCounter> &buckets,
                            double value) noexcept;
  void Downscale(uint32_t by) noexcept;

  mutable opentelemetry::common::SpinLockMutex lock_;
  Base2ExponentialHistogramPointData point_data_;
  Base2ExponentialHistogramIndexer indexer_;
  bool record_min_max_ = true;
};

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
