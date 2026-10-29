// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/data_to_shards_allocation_query_service.h"
#include "mongo/db/pipeline/historical_placement_fetcher.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo {

class DataToShardsAllocationQueryServiceImpl : public DataToShardsAllocationQueryService {
public:
    explicit DataToShardsAllocationQueryServiceImpl(
        std::unique_ptr<HistoricalPlacementFetcher> fetcher)
        : _fetcher(std::move(fetcher)) {}

    /**
     * Fetches the placement history for the whole cluster by calling '_fetcher' and returns the
     * corresponding HistoricalPlacementStatus.
     */
    AllocationToShardsStatus getAllocationToShardsStatus(OperationContext* opCtx,
                                                         const Timestamp& clusterTime) override;

private:
    std::unique_ptr<HistoricalPlacementFetcher> _fetcher;
};

}  // namespace mongo
