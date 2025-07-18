// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>

#include "opentelemetry/sdk/metrics/aggregation/aggregation.h"
#include "opentelemetry/sdk/metrics/data/point_data.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{

/**
 * A null Aggregation which denotes no aggregation should occur.
 */

class DropAggregation : public Aggregation
{
public:
  DropAggregation() = default;

  DropAggregation(const DropPointData &) {}

  void Aggregate(int64_t /* value */, const PointAttributes & /* attributes */) noexcept override {}

  void Aggregate(double /* value */, const PointAttributes & /* attributes */) noexcept override {}

  std::unique_ptr<Aggregation> Merge(const Aggregation &) const noexcept override
  {
    return std::unique_ptr<Aggregation>(new DropAggregation());
  }

  std::unique_ptr<Aggregation> Diff(const Aggregation &) const noexcept override
  {
    return std::unique_ptr<Aggregation>(new DropAggregation());
  }

  PointType ToPoint() const noexcept override
  {
    static DropPointData point_data;
    return point_data;
  }
};
}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
