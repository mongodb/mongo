// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include "opentelemetry/sdk/metrics/aggregation/base2_exponential_histogram_indexer.h"

#include <cmath>

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{

namespace
{

const double kLogBase2E = 1.0 / std::log(2.0);

double ComputeScaleFactor(int32_t scale)
{
  return std::scalbn(kLogBase2E, scale);
}

// Compute the bucket index using a logarithm based approach.
int32_t GetIndexByLogarithm(double value, double scale_factor)
{
  return static_cast<int32_t>(std::ceil(std::log(value) * scale_factor)) - 1;
}

// Compute the bucket index at scale 0.
int32_t MapToIndexScaleZero(double value)
{
  // Note: std::frexp() rounds submnormal values to the smallest normal value and returns an
  // exponent corresponding to fractions in the range [0.5, 1), whereas an exponent for the range
  // [1, 2), so subtract 1 from the exponent immediately.
  int exp;
  double frac = std::frexp(value, &exp);
  exp--;

  if (frac == 0.5)
  {
    // Special case for powers of two: they fall into the bucket numbered one less.
    exp--;
  }
  return exp;
}

}  // namespace

Base2ExponentialHistogramIndexer::Base2ExponentialHistogramIndexer(int32_t scale)
    : scale_(scale), scale_factor_(scale > 0 ? ComputeScaleFactor(scale) : 0)
{}

int32_t Base2ExponentialHistogramIndexer::ComputeIndex(double value) const
{
  const double abs_value = std::fabs(value);
  // For positive scales, compute the index by logarithm, which is simpler but may be
  // inaccurate near bucket boundaries
  if (scale_ > 0)
  {
    return GetIndexByLogarithm(abs_value, scale_factor_);
  }
  // For scale zero, compute the exact index by extracting the exponent.
  // For negative scales, compute the exact index by extracting the exponent and shifting it to
  // the right by -scale
  return MapToIndexScaleZero(abs_value) >> -scale_;
}

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
