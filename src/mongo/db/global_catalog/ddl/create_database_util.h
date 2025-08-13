/**
 *    Copyright (C) 2024-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/sharding_environment/shard_id.h"

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

BSONObj constructDbMatchFilterExact(StringData dbNameStr,
                                    const boost::optional<ShardId>& optResolvedPrimaryShard);

/**
 * Returns the database with the requested name and primary shard fields if existed.
 */
boost::optional<DatabaseType> findDatabaseExactMatch(
    OperationContext* opCtx,
    StringData dbNameStr,
    const boost::optional<ShardId>& optResolvedPrimaryShard);


BSONObj constructDbMatchFilterCaseInsensitive(StringData dbNameStr);

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
