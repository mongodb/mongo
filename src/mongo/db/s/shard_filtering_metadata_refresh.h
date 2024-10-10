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

#include <boost/optional/optional.hpp>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/database_version.h"
#include "mongo/s/shard_version.h"

namespace mongo {

/**
 * The `FilteringMetadataCache` class is responsible for storing and providing access to the current
 * sharding metadata for a given database or collection, and provides the functionality to refresh
 * it when necessary.
 */
class FilteringMetadataCache {
public:
    static void init(ServiceContext* serviceCtx);

    static FilteringMetadataCache* get(ServiceContext* serviceCtx);

    static FilteringMetadataCache* get(OperationContext* opCtx);

    /**
     * Must be invoked whenever code, which is executing on a shard encounters a StaleConfig error
     * and should be passed the placement version from the 'version received' in the exception. If
     * the shard's current placement version is behind 'chunkVersionReceived', causes the shard's
     * filtering metadata to be refreshed from the config server, otherwise does nothing and
     * immediately returns. If there are other threads currently performing refresh, blocks so that
     * only one of them hits the config server.
     *
     * If refresh fails for any reason (most commonly ExceededTimeLimit), returns a failed status.
     *
     * NOTE: Does network I/O and acquires collection lock on the specified namespace, so it must
     * not be called with a lock
     *
     * NOTE: This method is not expected to throw, because it is used in places where StaleConfig
     * exception was just caught and if it were to throw, it would overwrite any accumulated command
     * execution state in the response. This is specifically problematic for write commands, which
     * are expected to return the set of write batch entries that succeeded.
     */
    Status onCollectionPlacementVersionMismatchNoExcept(
        OperationContext* opCtx,
        const NamespaceString& nss,
        boost::optional<ChunkVersion> chunkVersionReceived) noexcept;

    void onCollectionPlacementVersionMismatch(OperationContext* opCtx,
                                              const NamespaceString& nss,
                                              boost::optional<ChunkVersion> chunkVersionReceived);

    /**
     * Should be called when any client request on this shard generates a StaleDbVersion exception.
     *
     * Invalidates the cached database version, schedules a refresh of the database info, waits for
     * the refresh to complete, and updates the cached database version.
     */
    Status onDbVersionMismatchNoExcept(OperationContext* opCtx,
                                       const DatabaseName& dbName,
                                       boost::optional<DatabaseVersion> clientDbVersion) noexcept;

    /**
     * Unconditionally get the shard's filtering metadata from the config server on the calling
     * thread. Returns the metadata if the nss is sharded, otherwise default unsharded metadata.
     *
     * NOTE: Does network I/O, so it must not be called with a lock
     */
    CollectionMetadata forceGetCurrentMetadata(OperationContext* opCtx, const NamespaceString& nss);

    /**
     * Unconditionally causes the shard's filtering metadata to be refreshed from the config server
     * and returns the resulting placement version (which might not have changed), or throws.
     *
     * NOTE: Does network I/O and acquires collection lock on the specified namespace, so it must
     * not be called with a lock
     */
    ChunkVersion forceShardFilteringMetadataRefresh(OperationContext* opCtx,
                                                    const NamespaceString& nss);

private:
    /**
     * Unconditionally refreshes the database metadata from the config server.
     *
     * NOTE: Does network I/O and acquires the database lock in X mode.
     */
    Status _refreshDbMetadata(OperationContext* opCtx,
                              const DatabaseName& dbName,
                              CancellationToken cancellationToken);

    SharedSemiFuture<void> _recoverRefreshDbVersion(OperationContext* opCtx,
                                                    const DatabaseName& dbName,
                                                    const CancellationToken& cancellationToken);

    void _onDbVersionMismatch(OperationContext* opCtx,
                              const DatabaseName& dbName,
                              boost::optional<DatabaseVersion> receivedDbVersion);

    SharedSemiFuture<void> _recoverRefreshCollectionPlacementVersion(
        ServiceContext* serviceContext,
        const NamespaceString& nss,
        bool runRecover,
        CancellationToken cancellationToken);
};

}  // namespace mongo
