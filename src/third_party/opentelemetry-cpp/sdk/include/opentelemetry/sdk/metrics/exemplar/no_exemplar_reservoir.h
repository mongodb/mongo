// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#ifdef ENABLE_METRICS_EXEMPLAR_PREVIEW

#  include <stdint.h>
#  include <memory>
#  include <vector>

#  include "opentelemetry/common/timestamp.h"
#  include "opentelemetry/context/context.h"
#  include "opentelemetry/sdk/metrics/data/exemplar_data.h"
#  include "opentelemetry/sdk/metrics/exemplar/filter_type.h"
#  include "opentelemetry/sdk/metrics/exemplar/reservoir.h"
#  include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{

class NoExemplarReservoir final : public ExemplarReservoir
{

public:
  void OfferMeasurement(
      int64_t /* value */,
      const MetricAttributes & /* attributes */,
      const opentelemetry::context::Context & /* context */,
      const opentelemetry::common::SystemTimestamp & /* timestamp */) noexcept override
  {
    // Stores nothing
  }

  void OfferMeasurement(
      double /* value */,
      const MetricAttributes & /* attributes */,
      const opentelemetry::context::Context & /* context */,
      const opentelemetry::common::SystemTimestamp & /* timestamp */) noexcept override
  {
    // Stores nothing.
  }

  std::vector<std::shared_ptr<ExemplarData>> CollectAndReset(
      const MetricAttributes & /* pointAttributes */) noexcept override
  {
    return std::vector<std::shared_ptr<ExemplarData>>{};
  }

  explicit NoExemplarReservoir() = default;
};

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE

#endif  // ENABLE_METRICS_EXEMPLAR_PREVIEW
