// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#ifdef ENABLE_METRICS_EXEMPLAR_PREVIEW

#  include <utility>

#  include "opentelemetry/sdk/metrics/exemplar/aligned_histogram_bucket_exemplar_reservoir.h"
#  include "opentelemetry/sdk/metrics/exemplar/no_exemplar_reservoir.h"
#  include "opentelemetry/sdk/metrics/exemplar/reservoir.h"
#  include "opentelemetry/sdk/metrics/exemplar/reservoir_cell.h"
#  include "opentelemetry/sdk/metrics/exemplar/simple_fixed_size_exemplar_reservoir.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace metrics
{

nostd::shared_ptr<ExemplarReservoir> ExemplarReservoir::GetSimpleFixedSizeExemplarReservoir(
    size_t size,
    std::shared_ptr<ReservoirCellSelector> reservoir_cell_selector,
    MapAndResetCellType map_and_reset_cell)
{
  return nostd::shared_ptr<ExemplarReservoir>{new SimpleFixedSizeExemplarReservoir{
      size, std::move(reservoir_cell_selector), map_and_reset_cell}};
}

nostd::shared_ptr<ExemplarReservoir> ExemplarReservoir::GetAlignedHistogramBucketExemplarReservoir(
    size_t size,
    std::shared_ptr<ReservoirCellSelector> reservoir_cell_selector,
    MapAndResetCellType map_and_reset_cell)
{
  return nostd::shared_ptr<ExemplarReservoir>{new AlignedHistogramBucketExemplarReservoir{
      size, std::move(reservoir_cell_selector), map_and_reset_cell}};
}

nostd::shared_ptr<ExemplarReservoir> ExemplarReservoir::GetNoExemplarReservoir()
{
  return nostd::shared_ptr<ExemplarReservoir>{new NoExemplarReservoir{}};
}

}  // namespace metrics
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE

#endif  // ENABLE_METRICS_EXEMPLAR_PREVIEW
