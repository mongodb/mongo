// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/data_to_shards_allocation_query_service.h"

#include "mongo/util/decorable.h"

namespace mongo {
namespace {
const auto getDataToShardsAllocationQueryServiceFromServiceContext =
    ServiceContext::declareDecoration<std::unique_ptr<DataToShardsAllocationQueryService>>();

}  // namespace

DataToShardsAllocationQueryService* DataToShardsAllocationQueryService::get(
    ServiceContext* service) {
    return getDataToShardsAllocationQueryServiceFromServiceContext(service).get();
}

DataToShardsAllocationQueryService* DataToShardsAllocationQueryService::get(
    OperationContext* opCtx) {
    return get(opCtx->getServiceContext());
}

void DataToShardsAllocationQueryService::set(
    ServiceContext* service,
    std::unique_ptr<DataToShardsAllocationQueryService> dataToShardsAllocationQueryService) {
    auto& holder = getDataToShardsAllocationQueryServiceFromServiceContext(service);
    holder = std::move(dataToShardsAllocationQueryService);
}

std::unique_ptr<DataToShardsAllocationQueryService>
DataToShardsAllocationQueryService::swap_forTest(
    ServiceContext* service, std::unique_ptr<DataToShardsAllocationQueryService> replacement) {
    auto& holder = getDataToShardsAllocationQueryServiceFromServiceContext(service);
    auto oldService = std::move(holder);
    holder = std::move(replacement);
    return oldService;
}

}  // namespace mongo
