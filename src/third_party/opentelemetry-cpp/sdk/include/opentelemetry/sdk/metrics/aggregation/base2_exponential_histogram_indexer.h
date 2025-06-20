// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "opentelemetry/version.h"

#include <cstdint>

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{

/*
 * An indexer for base2 exponential histograms. It is used to calculate index for a given value and
 * scale.
 */
class Base2ExponentialHistogramIndexer
{
public:
  /*
   * Construct a new indexer for a given scale.
   */
  explicit Base2ExponentialHistogramIndexer(int32_t scale = 0);
  Base2ExponentialHistogramIndexer(const Base2ExponentialHistogramIndexer &other) = default;
  Base2ExponentialHistogramIndexer &operator=(const Base2ExponentialHistogramIndexer &other) =
      default;

  /**
   * Compute the index for the given value.
   *
   * @param value Measured value (must be non-zero).
   * @return the index of the bucket which the value maps to.
   */
  int32_t ComputeIndex(double value) const;

private:
  int32_t scale_;
  double scale_factor_;
};

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
