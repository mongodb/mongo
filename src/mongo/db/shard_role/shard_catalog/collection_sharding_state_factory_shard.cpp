// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/collection_sharding_state_factory_shard.h"

#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

CollectionShardingStateFactoryShard::CollectionShardingStateFactoryShard(
    ServiceContext* serviceContext)
    : _serviceContext(serviceContext) {}

std::unique_ptr<CollectionShardingState> CollectionShardingStateFactoryShard::make(
    const NamespaceString& nss) {
    return std::make_unique<CollectionShardingRuntime>(_serviceContext, nss);
}

const StaleShardCollectionMetadataHandler&
CollectionShardingStateFactoryShard::getStaleShardExceptionHandler() const {
    return _staleExceptionHandler;
}

}  // namespace mongo
