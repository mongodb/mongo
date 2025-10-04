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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache.h"
#include "mongo/db/global_catalog/catalog_cache/shard_server_catalog_cache_loader.h"
#include "mongo/db/local_catalog/shard_role_catalog/collection_metadata.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/util/fail_point.h"

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * The `FilteringMetadataCache` class is responsible for storing and providing access to the current
 * sharding metadata for a given database or collection, and provides the functionality to refresh
 * it when necessary.
 */
class FilteringMetadataCache {
public:
    FilteringMetadataCache() = default;

    static void init(ServiceContext* serviceCtx,
                     std::shared_ptr<ShardServerCatalogCacheLoader> loader,
                     bool isPrimary);
    static void initForTesting(ServiceContext* serviceCtx,
                               std::shared_ptr<ShardServerCatalogCacheLoader> loader);

    static FilteringMetadataCache* get(ServiceContext* serviceCtx);

    static FilteringMetadataCache* get(OperationContext* opCtx);

    /**
     * Shuts down and joins the executors used by the internal components.
     */
    void shutDown();

    /**
     * Updates internal state so that the loader can start behaving like a secondary.
     */
    void onStepDown();

    /**
     * Updates internal state so that the loader can start behaving like a primary.
     */
    void onStepUp();

    /**
     * Interrupts ongoing refreshes to prevent secondaries from waiting for opTimes from wrong terms
     * in case of rollback. Primaries must step down before going through rollback, so this should
     * only be run on secondaries.
     */
    void onReplicationRollback();

    /**
     * Sets any notifications waiting for this version to arrive and invalidates the catalog cache's
     * chunk metadata for collection 'nss' so that the next caller provokes a refresh.
     */
    void notifyOfCollectionRefreshEndMarkerSeen(const NamespaceString& nss,
                                                const Timestamp& commitTime);

    /**
     * Waits for any pending changes for the specified database or collection to be persisted
     * locally (not necessarily majority replicated). If newer changes come after this method has
     * started running, they will not be waited for except if there is a drop.
     *
     * May throw if the node steps down from primary or if the operation time is exceeded or due to
     * any other error condition.
     *
     * If the specific loader implementation does not support persistence, these methods are
     * undefined and must fassert.
     */
    void waitForCollectionFlush(OperationContext* opCtx, const NamespaceString& nss);

    void waitForDatabaseFlush(OperationContext* opCtx, const DatabaseName& dbName);

    /**
     * Reports statistics about the catalog cache to be used by serverStatus.
     */
    void report(BSONObjBuilder* builder) const;

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
    Status onCollectionPlacementVersionMismatch(
        OperationContext* opCtx,
        const NamespaceString& nss,
        boost::optional<ChunkVersion> chunkVersionReceived) noexcept;

    /**
     * Unconditionally causes the collection placement to be refreshed from the config server.
     *
     * NOTE: Does network I/O and acquires collection lock on the specified namespace, so it must
     * not be called with a lock.
     */
    void forceCollectionPlacementRefresh(OperationContext* opCtx, const NamespaceString& nss);

    /**
     * Should be called when any client request on this shard generates a StaleDbVersion exception.
     *
     * Invalidates the cached database version, schedules a refresh of the database info, waits for
     * the refresh to complete, and updates the cached database version.
     */
    Status onDbVersionMismatch(OperationContext* opCtx,
                               const DatabaseName& dbName,
                               const DatabaseVersion& clientDbVersion) noexcept;

    /**
     * Unconditionally causes the database metadata to be refreshed from the config server.
     *
     * NOTE: This method is deprecated and should not be used. In the authoritative model, database
     * refreshes are not required, as shards are authoritative for the databases that own. This
     * method is retained for backward compatibility.
     */
    Status forceDatabaseMetadataRefresh_DEPRECATED(OperationContext* opCtx,
                                                   const DatabaseName& dbName) noexcept;

    /**
     * Clears the filtering metadata cache object so that it can be reused between test executions.
     *
     * NOTE: Do not use this outside of unit-tests.
     */
    void clearForUnitTests() {
        _cache.reset();
    }

private:
    /**
     * Unconditionally get the shard's filtering metadata from the config server on the calling
     * thread. Returns the metadata if the nss is sharded, otherwise default unsharded metadata.
     *
     * NOTE: Does network I/O, so it must not be called with a lock
     */
    CollectionMetadata _forceGetCurrentCollectionMetadata(OperationContext* opCtx,
                                                          const NamespaceString& nss);

    /**
     * Drive each unfished migration coordination in the given namespace to completion.
     * Assumes the caller to have entered CollectionCriticalSection.
     */
    void _recoverMigrationCoordinations(OperationContext* opCtx,
                                        NamespaceString nss,
                                        CancellationToken cancellationToken);

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

    void _onDbVersionMismatchAuthoritative(OperationContext* opCtx,
                                           const DatabaseName& dbName,
                                           const DatabaseVersion& receivedDbVersion);

    SharedSemiFuture<void> _recoverRefreshCollectionPlacementVersion(
        ServiceContext* serviceContext,
        const NamespaceString& nss,
        bool runRecover,
        CancellationToken cancellationToken);

    void _onCollectionPlacementVersionMismatch(OperationContext* opCtx,
                                               const NamespaceString& nss,
                                               boost::optional<ChunkVersion> chunkVersionReceived);

    // TODO (SERVER-97261): remove the Grid's CatalogCache usages once 9.0 becomes last LTS.
    // If _cache is set, it will be used only for filtering; otherwise, the Grid's CatalogCache will
    // be used.
    std::unique_ptr<CatalogCache> _cache;
    std::shared_ptr<ShardServerCatalogCacheLoader> _loader;
};

extern FailPoint hangInRefreshFilteringMetadataUntilSuccessInterruptible;
extern FailPoint hangInRefreshFilteringMetadataUntilSuccessThenSimulateErrorUninterruptible;

}  // namespace mongo
