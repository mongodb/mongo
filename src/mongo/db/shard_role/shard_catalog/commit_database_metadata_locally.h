// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {

namespace [[MONGO_MOD_PARENT_PRIVATE]] shard_catalog_commit {

void writeDatabaseMetadataOplogEntry(OperationContext* opCtx,
                                     repl::MutableOplogEntry& oplogEntry,
                                     std::string_view commandName);

/**
 * Persists the database metadata into the shard catalog (config.shard.catalog.databases), writes an
 * oplog 'c' entry to inform secondaries on how to populate the DatabaseShardingState (DSS), and
 * applies that same entry locally on this (primary) node via the op observer's
 * onCreateDatabaseMetadata hook.
 */
void commitCreateDatabaseMetadataLocally(OperationContext* opCtx,
                                         const DatabaseType& dbMetadata,
                                         bool fromClone = false);

/**
 * Deletes the database metadata from the shard catalog (config.shard.catalog.databases), writes an
 * oplog 'c' entry to invalidate the DatabaseShardingState (DSS) on secondaries, and clears the
 * in-memory DatabaseShardingRuntime (DSR) on this (primary) node.
 */
void commitDropDatabaseMetadataLocally(OperationContext* opCtx, const DatabaseName& dbName);

}  // namespace shard_catalog_commit
}  // namespace mongo
