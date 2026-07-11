// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/database_name.h"
#include "mongo/db/router_role/routing_cache/catalog_cache_loader.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Shard implementation of the CatalogCacheLoader used by the CatalogCache. Retrieves chunk metadata
 * for the CatalogCache on shards.
 *
 * If a shard primary, retrieves chunk metadata from the config server and maintains a persisted
 * copy of that chunk metadata so shard secondaries can access the metadata. If a shard secondary,
 * retrieves chunk metadata from the shard persisted chunk metadata.
 */
class [[MONGO_MOD_PARENT_PRIVATE]] ShardServerCatalogCacheLoader : public CatalogCacheLoader {
public:
    ~ShardServerCatalogCacheLoader() override = default;

    /**
     * Initializes internal state so that the loader behaves as a primary or secondary. This can
     * only be called once, when the sharding state is initialized.
     */
    virtual void initializeReplicaSetRole(bool isPrimary) = 0;

    /**
     * Updates internal state so that the loader can start behaving like a secondary.
     */
    virtual void onStepDown() = 0;

    /**
     * Updates internal state so that the loader can start behaving like a primary.
     */
    virtual void onStepUp() = 0;

    /**
     * Interrupts ongoing refreshes to prevent secondaries from waiting for opTimes from wrong terms
     * in case of rollback. Primaries must step down before going through rollback, so this should
     * only be run on secondaries.
     */
    virtual void onReplicationRollback() = 0;

    /**
     * Sets any notifications waiting for this version to arrive and invalidates the catalog cache's
     * chunk metadata for collection 'nss' so that the next caller provokes a refresh.
     */
    virtual void notifyOfCollectionRefreshEndMarkerSeen(const NamespaceString& nss,
                                                        const Timestamp& commitTime) = 0;

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
    virtual void waitForCollectionFlush(OperationContext* opCtx, const NamespaceString& nss) = 0;

    virtual void waitForDatabaseFlush(OperationContext* opCtx, const DatabaseName& dbName) = 0;

    /**
     * Interrupts all in-flight loads with error code `MetadataRefreshCanceledDueToFCVTransition`
     * after the node has become authoritative for the sharding metadata.
     * TODO (SERVER-98118): remove once 9.0 becomes last LTS.
     */
    virtual void interruptAfterAuthoritativeShardsTransition() = 0;

    /**
     * Waits for all enqueued collection and database metadata persistence tasks to complete
     * after the node has become authoritative for the sharding metadata.
     * TODO (SERVER-98118): remove once 9.0 becomes last LTS.
     */
    virtual void waitForAllFlushes(OperationContext* opCtx) = 0;
};

}  // namespace mongo
