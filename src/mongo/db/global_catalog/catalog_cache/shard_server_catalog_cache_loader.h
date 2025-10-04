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

#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache_loader.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/util/future.h"

namespace mongo {

/**
 * Shard implementation of the CatalogCacheLoader used by the CatalogCache. Retrieves chunk metadata
 * for the CatalogCache on shards.
 *
 * If a shard primary, retrieves chunk metadata from the config server and maintains a persisted
 * copy of that chunk metadata so shard secondaries can access the metadata. If a shard secondary,
 * retrieves chunk metadata from the shard persisted chunk metadata.
 */
class ShardServerCatalogCacheLoader : public CatalogCacheLoader {
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
};

}  // namespace mongo
