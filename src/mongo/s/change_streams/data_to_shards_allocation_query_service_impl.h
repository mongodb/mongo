/**
 *    Copyright (C) 2025-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/db/pipeline/data_to_shards_allocation_query_service.h"
#include "mongo/db/pipeline/historical_placement_fetcher.h"

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
