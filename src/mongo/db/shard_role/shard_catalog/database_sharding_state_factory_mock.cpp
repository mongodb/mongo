// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/database_sharding_state_factory_mock.h"

#include "mongo/db/shard_role/shard_catalog/database_sharding_state_mock.h"

namespace mongo {

std::unique_ptr<DatabaseShardingState> DatabaseShardingStateFactoryMock::make(
    const DatabaseName& dbName) {
    return std::make_unique<DatabaseShardingStateMock>(dbName);
}

const StaleShardDatabaseMetadataHandler&
DatabaseShardingStateFactoryMock::getStaleShardExceptionHandler() const {
    MONGO_UNIMPLEMENTED;
}

}  // namespace mongo
