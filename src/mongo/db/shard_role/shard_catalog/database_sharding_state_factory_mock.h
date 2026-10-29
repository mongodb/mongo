// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/database_name.h"
#include "mongo/db/shard_role/shard_catalog/database_sharding_state.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo {

class [[MONGO_MOD_PUBLIC]] DatabaseShardingStateFactoryMock final
    : public DatabaseShardingStateFactory {
public:
    DatabaseShardingStateFactoryMock() = default;

    std::unique_ptr<DatabaseShardingState> make(const DatabaseName& dbName) override;

    const StaleShardDatabaseMetadataHandler& getStaleShardExceptionHandler() const override;
};

}  // namespace mongo
