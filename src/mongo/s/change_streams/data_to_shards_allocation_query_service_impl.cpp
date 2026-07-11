// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/change_streams/data_to_shards_allocation_query_service_impl.h"

#include "mongo/db/namespace_string.h"
#include "mongo/db/service_context.h"
#include "mongo/s/change_streams/data_to_shards_allocation_query_service_impl.h"
#include "mongo/s/change_streams/historical_placement_fetcher_impl.h"
#include "mongo/util/assert_util.h"

namespace mongo {

AllocationToShardsStatus DataToShardsAllocationQueryServiceImpl::getAllocationToShardsStatus(
    OperationContext* opCtx, const Timestamp& clusterTime) {
    switch (_fetcher
                ->fetch(opCtx,
                        NamespaceString::kEmpty,
                        clusterTime,
                        true /* checkIfPointInTimeIsInFuture */,
                        false /* ignoreRemovedShards */)
                .getStatus()) {
        case HistoricalPlacementStatus::OK:
            return AllocationToShardsStatus::kOk;
        case HistoricalPlacementStatus::FutureClusterTime:
            return AllocationToShardsStatus::kFutureClusterTime;
        case HistoricalPlacementStatus::NotAvailable:
            return AllocationToShardsStatus::kNotAvailable;
    }

    MONGO_UNREACHABLE_TASSERT(10718905);
}

namespace {

ServiceContext::ConstructorActionRegisterer dataToShardsAllocationQueryServiceRegisterer(
    "DataToShardsAllocationQueryServiceRegisterer",
    {},
    [](ServiceContext* serviceContext) {
        invariant(serviceContext);

        auto fetcher = std::make_unique<HistoricalPlacementFetcherImpl>();
        DataToShardsAllocationQueryService::set(
            serviceContext,
            std::make_unique<DataToShardsAllocationQueryServiceImpl>(std::move(fetcher)));
    },
    {});
}  // namespace

}  // namespace mongo
