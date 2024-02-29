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

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <functional>
#include <memory>
#include <vector>

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog/type_database_gen.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/s/type_collection_common_types_gen.h"
#include "mongo/util/future.h"
#include "mongo/util/uuid.h"

namespace mongo {

/**
 * Interface through which the sharding catalog cache requests the set of changed chunks to be
 * retrieved from the persisted metadata store.
 */
class CatalogCacheLoader {
public:
    virtual ~CatalogCacheLoader();

    /**
     * Stores a loader on the specified service context. May only be called once for the lifetime of
     * the service context.
     */
    static void set(ServiceContext* serviceContext, std::unique_ptr<CatalogCacheLoader> loader);

    static CatalogCacheLoader& get(ServiceContext* serviceContext);
    static CatalogCacheLoader& get(OperationContext* opCtx);

    /**
     * Used as a return value for getChunksSince.
     */
    struct CollectionAndChangedChunks {
        CollectionAndChangedChunks();
        CollectionAndChangedChunks(
            OID epoch,
            Timestamp timestamp,
            UUID uuid,
            bool unsplittable,
            const BSONObj& collShardKeyPattern,
            const BSONObj& collDefaultCollation,
            bool collShardKeyIsUnique,
            boost::optional<TypeCollectionTimeseriesFields> collTimeseriesFields,
            boost::optional<TypeCollectionReshardingFields> collReshardingFields,
            bool allowMigrations,
            std::vector<ChunkType> chunks);

        // Information about the entire collection
        OID epoch;
        Timestamp timestamp;
        boost::optional<UUID> uuid;  // This value can never be boost::none,
                                     // except under the default constructor

        bool unsplittable;

        BSONObj shardKeyPattern;
        BSONObj defaultCollation;
        bool shardKeyIsUnique;

        // This information will be valid if the collection is a time-series buckets collection.
        boost::optional<TypeCollectionTimeseriesFields> timeseriesFields;

        // If the collection is currently undergoing a resharding operation, the optional will be
        // populated.
        boost::optional<TypeCollectionReshardingFields> reshardingFields;

        bool allowMigrations;

        // The chunks which have changed sorted by their chunkVersion. This list might potentially
        // contain all the chunks in the collection.
        std::vector<ChunkType> changedChunks;
    };

    using GetChunksSinceCallbackFn =
        std::function<void(OperationContext*, StatusWith<CollectionAndChangedChunks>)>;

    /**
     * Initializes internal state. Must be called only once when sharding state is initialized.
     */
    virtual void initializeReplicaSetRole(bool isPrimary) = 0;

    /**
     * Changes internal state on step down.
     */
    virtual void onStepDown() = 0;

    /**
     * Changes internal state on step up.
     */
    virtual void onStepUp() = 0;

    /**
     * Interrupts ongoing refreshes on rollback.
     */
    virtual void onReplicationRollback() = 0;

    /**
     * Transitions into shut down and cleans up state. Once this transitions to shut down, should
     * not be able to transition back to normal. Should be safe to be called more than once.
     */
    virtual void shutDown() = 0;

    /**
     * Notifies the loader that the persisted collection placement version for 'nss' has been
     * updated. `commitTime` represents the commit time of the update to config.collections for
     * `nss` setting the refreshing flag to false.
     */
    virtual void notifyOfCollectionRefreshEndMarkerSeen(const NamespaceString& nss,
                                                        const Timestamp& commitTime) = 0;

    /**
     * Non-blocking call, which returns the chunks changed since the specified version to be
     * fetched from the persistent metadata store.
     *
     * If for some reason the asynchronous fetch operation cannot be dispatched (for example on
     * shutdown), throws a DBException.
     */
    virtual SemiFuture<CollectionAndChangedChunks> getChunksSince(const NamespaceString& nss,
                                                                  ChunkVersion version) = 0;

    /**
     * Non-blocking call, which returns the most recent db version for the given dbName from the
     * the persistent metadata store.
     *
     * If for some reason the asynchronous fetch operation cannot be dispatched (for example on
     * shutdown), throws a DBException.
     */
    virtual SemiFuture<DatabaseType> getDatabase(const DatabaseName& dbName) = 0;

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

protected:
    CatalogCacheLoader();
};

}  // namespace mongo

#define LOGV2_FOR_CATALOG_REFRESH(ID, DLEVEL, MESSAGE, ...) \
    LOGV2_DEBUG_OPTIONS(                                    \
        ID, DLEVEL, {logv2::LogComponent::kShardingCatalogRefresh}, MESSAGE, ##__VA_ARGS__)
