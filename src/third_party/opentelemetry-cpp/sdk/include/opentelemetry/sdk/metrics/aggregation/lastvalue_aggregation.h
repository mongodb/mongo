// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <stdint.h>
#include <memory>

#include "opentelemetry/common/spin_lock_mutex.h"
#include "opentelemetry/sdk/metrics/aggregation/aggregation.h"
#include "opentelemetry/sdk/metrics/data/metric_data.h"
#include "opentelemetry/sdk/metrics/data/point_data.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{
class LongLastValueAggregation : public Aggregation
{
public:
  LongLastValueAggregation();
  LongLastValueAggregation(LastValuePointData &&);
  LongLastValueAggregation(const LastValuePointData &);

  void Aggregate(int64_t value, const PointAttributes &attributes = {}) noexcept override;

  void Aggregate(double /* value */, const PointAttributes & /* attributes */) noexcept override {}

  std::unique_ptr<Aggregation> Merge(const Aggregation &delta) const noexcept override;

  std::unique_ptr<Aggregation> Diff(const Aggregation &next) const noexcept override;

  PointType ToPoint() const noexcept override;

private:
  mutable opentelemetry::common::SpinLockMutex lock_;
  LastValuePointData point_data_;
};

class DoubleLastValueAggregation : public Aggregation
{
public:
  DoubleLastValueAggregation();
  DoubleLastValueAggregation(LastValuePointData &&);
  DoubleLastValueAggregation(const LastValuePointData &);

  void Aggregate(int64_t /* value */, const PointAttributes & /* attributes */) noexcept override {}

  void Aggregate(double value, const PointAttributes &attributes = {}) noexcept override;

  virtual std::unique_ptr<Aggregation> Merge(const Aggregation &delta) const noexcept override;

  virtual std::unique_ptr<Aggregation> Diff(const Aggregation &next) const noexcept override;

  PointType ToPoint() const noexcept override;

private:
  mutable opentelemetry::common::SpinLockMutex lock_;
  mutable LastValuePointData point_data_;
};

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
