// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/database_sharding_state_factory_shard.h"

#include "mongo/db/shard_role/shard_catalog/database_sharding_runtime.h"

namespace mongo {

std::unique_ptr<DatabaseShardingState> DatabaseShardingStateFactoryShard::make(
    const DatabaseName& dbName) {
    return std::make_unique<DatabaseShardingRuntime>(dbName);
}

const StaleShardDatabaseMetadataHandler&
DatabaseShardingStateFactoryShard::getStaleShardExceptionHandler() const {
    return _staleExceptionHandler;
}

}  // namespace mongo
