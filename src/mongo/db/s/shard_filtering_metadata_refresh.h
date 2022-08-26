/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/namespace_string.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/s/database_version.h"
#include "mongo/s/shard_version.h"

namespace mongo {

class OperationContext;

/**
 * Must be invoked whenever code, which is executing on a shard encounters a StaleConfig exception
 * and should be passed the 'version received' from the exception. If the shard's current version is
 * behind 'shardVersionReceived', causes the shard's filtering metadata to be refreshed from the
 * config server, otherwise does nothing and immediately returns. If there are other threads
 * currently performing refresh, blocks so that only one of them hits the config server.
 *
 * If refresh fails for any reason (most commonly ExceededTimeLimit), returns a failed status.
 *
 * NOTE: Does network I/O and acquires collection lock on the specified namespace, so it must not be
 * called with a lock
 *
 * NOTE: This method is not expected to throw, because it is used in places where StaleConfig
 * exception was just caught and if it were to throw, it would overwrite any accumulated command
 * execution state in the response. This is specifically problematic for write commands, which are
 * expected to return the set of write batch entries that succeeded.
 */
Status onShardVersionMismatchNoExcept(OperationContext* opCtx,
                                      const NamespaceString& nss,
                                      boost::optional<ShardVersion> shardVersionReceived) noexcept;

void onShardVersionMismatch(OperationContext* opCtx,
                            const NamespaceString& nss,
                            boost::optional<ShardVersion> shardVersionReceived);

/**
 * Unconditionally get the shard's filtering metadata from the config server on the calling thread.
 * Returns the metadata if the nss is sharded, otherwise default unsharded metadata.
 *
 * NOTE: Does network I/O, so it must not be called with a lock
 */
CollectionMetadata forceGetCurrentMetadata(OperationContext* opCtx, const NamespaceString& nss);

/**
 * Unconditionally causes the shard's filtering metadata to be refreshed from the config server and
 * returns the resulting shard version (which might not have changed), or throws.
 *
 * NOTE: Does network I/O and acquires collection lock on the specified namespace, so it must not be
 * called with a lock
 */
ChunkVersion forceShardFilteringMetadataRefresh(OperationContext* opCtx,
                                                const NamespaceString& nss);

/**
 * Should be called when any client request on this shard generates a StaleDbVersion exception.
 *
 * Invalidates the cached database version, schedules a refresh of the database info, waits for the
 * refresh to complete, and updates the cached database version.
 */
Status onDbVersionMismatchNoExcept(OperationContext* opCtx,
                                   StringData dbName,
                                   boost::optional<DatabaseVersion> clientDbVersion) noexcept;

void forceDatabaseRefresh(OperationContext* opCtx, StringData dbName);

}  // namespace mongo
