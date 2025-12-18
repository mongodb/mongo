// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <vector>

#include "opentelemetry/sdk/metrics/instruments.h"
#include "opentelemetry/sdk/metrics/state/attributes_hashmap.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{

class AggregationConfig
{
public:
  AggregationConfig(size_t cardinality_limit = kAggregationCardinalityLimit)
      : cardinality_limit_(cardinality_limit)
  {}

  virtual AggregationType GetType() const noexcept { return AggregationType::kDefault; }

  static const AggregationConfig *GetOrDefault(const AggregationConfig *config)
  {
    if (config)
    {
      return config;
    }
    static const AggregationConfig default_config{};
    return &default_config;
  }

  size_t cardinality_limit_;
  virtual ~AggregationConfig() = default;
};

class HistogramAggregationConfig : public AggregationConfig
{
public:
  HistogramAggregationConfig(size_t cardinality_limit = kAggregationCardinalityLimit)
      : AggregationConfig(cardinality_limit)
  {}

  AggregationType GetType() const noexcept override { return AggregationType::kHistogram; }

  std::vector<double> boundaries_;
  bool record_min_max_ = true;
};

class Base2ExponentialHistogramAggregationConfig : public AggregationConfig
{
public:
  Base2ExponentialHistogramAggregationConfig(
      size_t cardinality_limit = kAggregationCardinalityLimit)
      : AggregationConfig(cardinality_limit)
  {}

  AggregationType GetType() const noexcept override
  {
    return AggregationType::kBase2ExponentialHistogram;
  }

  size_t max_buckets_  = 160;
  int32_t max_scale_   = 20;
  bool record_min_max_ = true;
};

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
