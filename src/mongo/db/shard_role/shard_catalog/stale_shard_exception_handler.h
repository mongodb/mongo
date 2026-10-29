// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_state.h"
#include "mongo/db/shard_role/shard_catalog/database_sharding_state.h"
#include "mongo/db/versioning_protocol/stale_exception.h"
#include "mongo/util/modules.h"

namespace mongo {

class StaleShardCollectionMetadataHandlerImpl : public StaleShardCollectionMetadataHandler {
public:
    boost::optional<ChunkVersion> handleStaleShardVersionException(
        OperationContext* opCtx, const StaleConfigInfo& sci) const override;
};

class StaleShardDatabaseMetadataHandlerImpl : public StaleShardDatabaseMetadataHandler {
public:
    boost::optional<DatabaseVersion> handleStaleDatabaseVersionException(
        OperationContext* opCtx, const StaleDbRoutingVersion& staleDbException) const override;
};

}  // namespace mongo
