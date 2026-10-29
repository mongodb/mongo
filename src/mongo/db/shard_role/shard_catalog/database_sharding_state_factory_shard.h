// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/database_name.h"
#include "mongo/db/shard_role/shard_catalog/database_sharding_state.h"
#include "mongo/db/shard_role/shard_catalog/stale_shard_exception_handler.h"
#include "mongo/util/modules.h"

namespace mongo {

class [[MONGO_MOD_PUBLIC]] DatabaseShardingStateFactoryShard final
    : public DatabaseShardingStateFactory {
public:
    DatabaseShardingStateFactoryShard() = default;

    std::unique_ptr<DatabaseShardingState> make(const DatabaseName& dbName) override;

    const StaleShardDatabaseMetadataHandler& getStaleShardExceptionHandler() const override;

private:
    StaleShardDatabaseMetadataHandlerImpl _staleExceptionHandler;
};

}  // namespace mongo
