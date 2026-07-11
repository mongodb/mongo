// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/optional/optional.hpp>

namespace mongo {

// TODO (SERVER-94362): Inline these util functions in create_database_coordinator.cpp.
namespace create_database_util {

void logCommitCreateDatabaseFailed(const DatabaseName& dbName, const std::string& reason);

/**
 * Throws an error if the requested dbName to be created violates the constraints.
 * If 'config' database is requested, simply returns 'config' database on the config server.
 */
boost::optional<DatabaseType> checkDbNameConstraints(const DatabaseName& dbName);

/**
 * Resolves the shard against the received parameter which may encode either a shard ID or a
 * connection string.
 */
boost::optional<ShardId> resolvePrimaryShard(OperationContext* opCtx,
                                             const boost::optional<ShardId>& optPrimaryShard);

BSONObj constructDbMatchFilterExact(std::string_view dbNameStr,
                                    const boost::optional<ShardId>& optResolvedPrimaryShard);

/**
 * Returns the database with the requested name and primary shard fields if existed.
 */
boost::optional<DatabaseType> findDatabaseExactMatch(
    OperationContext* opCtx,
    std::string_view dbNameStr,
    const boost::optional<ShardId>& optResolvedPrimaryShard);


BSONObj constructDbMatchFilterCaseInsensitive(std::string_view dbNameStr);

void checkAgainstExistingDbDoc(const DatabaseType& existingDb,
                               const DatabaseName& dbName,
                               const boost::optional<ShardId>& optResolvedPrimaryShard);

/**
 * Checks for existing database that only differs in case or has a conflicting primary shard
 * option.
 * Returns the existing database if possible.
 */
boost::optional<DatabaseType> checkForExistingDatabaseWithDifferentOptions(
    OperationContext* opCtx,
    const DatabaseName& dbName,
    const boost::optional<ShardId>& optResolvedPrimaryShard);

/**
 * Checks if there is a document with {_id: dbName} in 'config.dropPendingDBs' collection, which is
 * used to serialize dropDatabase followed by a concurrent createDatabase correctly.
 */
bool checkIfDropPendingDB(OperationContext* opCtx, const DatabaseName& dbName);

/**
 * If there was no explicit dbPrimary shard choosen by the caller, then select the least loaded
 * non-draining shard.
 */
ShardId getCandidatePrimaryShard(OperationContext* opCtx,
                                 const boost::optional<ShardId>& optResolvedPrimaryShard);

void refreshDbVersionOnPrimaryShard(OperationContext* opCtx,
                                    const std::string& dbNameStr,
                                    const ShardId& primaryShard);

}  // namespace create_database_util
}  // namespace mongo
