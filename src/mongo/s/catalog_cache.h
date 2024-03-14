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

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <cstdint>
#include <string>
#include <utility>

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_id.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/s/catalog/type_database_gen.h"
#include "mongo/s/catalog/type_index_catalog.h"
#include "mongo/s/catalog_cache_loader.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/database_version.h"
#include "mongo/s/shard_version.h"
#include "mongo/s/sharding_index_catalog_cache.h"
#include "mongo/s/type_collection_common_types_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/concurrency/thread_pool_interface.h"
#include "mongo/util/read_through_cache.h"

namespace mongo {

static constexpr int kMaxNumStaleVersionRetries = 10;

class ComparableDatabaseVersion;

using DatabaseTypeCache = ReadThroughCache<DatabaseName, DatabaseType, ComparableDatabaseVersion>;
using DatabaseTypeValueHandle = DatabaseTypeCache::ValueHandle;
using CachedDatabaseInfo = DatabaseTypeValueHandle;

struct CollectionRoutingInfo {
    CollectionRoutingInfo(ChunkManager&& chunkManager,
                          boost::optional<ShardingIndexesCatalogCache>&& shardingIndexesCatalog)
        : cm(std::move(chunkManager)), sii(std::move(shardingIndexesCatalog)) {}
    ChunkManager cm;
    boost::optional<ShardingIndexesCatalogCache> sii;

    ShardVersion getCollectionVersion() const;
    ShardVersion getShardVersion(const ShardId& shardId) const;
};

/**
 * This class wrap a DatabaseVersion object augmenting it with:
 *  - a sequence number to allow for forced catalog cache refreshes
 *  - a sequence number to disambiguate scenarios in which the DatabaseVersion isn't valid
 */
class ComparableDatabaseVersion {
public:
    /**
     * Creates a ComparableDatabaseVersion that wraps the given DatabaseVersion.
     *
     * If version is boost::none it creates a ComparableDatabaseVersion that doesn't have a valid
     * version. This is useful in some scenarios in which the DatabaseVersion is provided later
     * through ComparableDatabaseVersion::setVersion or to represent that a Database doesn't exist
     */
    static ComparableDatabaseVersion makeComparableDatabaseVersion(
        const boost::optional<DatabaseVersion>& version);

    /**
     * Creates a new instance which will artificially be greater than any
     * previously created ComparableDatabaseVersion and smaller than any instance
     * created afterwards. Used as means to cause the collections cache to
     * attempt a refresh in situations where causal consistency cannot be
     * inferred.
     */
    static ComparableDatabaseVersion makeComparableDatabaseVersionForForcedRefresh();

    /**
     * Empty constructor needed by the ReadThroughCache.
     *
     * Instances created through this constructor will be always less than the ones created through
     * the static constructor.
     */
    ComparableDatabaseVersion() = default;

    std::string toString() const;

    bool operator==(const ComparableDatabaseVersion& other) const;

    bool operator!=(const ComparableDatabaseVersion& other) const {
        return !(*this == other);
    }

    bool operator<(const ComparableDatabaseVersion& other) const;

    bool operator>(const ComparableDatabaseVersion& other) const {
        return other < *this;
    }

    bool operator<=(const ComparableDatabaseVersion& other) const {
        return !(*this > other);
    }

    bool operator>=(const ComparableDatabaseVersion& other) const {
        return !(*this < other);
    }

private:
    friend class CatalogCache;

    static AtomicWord<uint64_t> _disambiguatingSequenceNumSource;
    static AtomicWord<uint64_t> _forcedRefreshSequenceNumSource;

    ComparableDatabaseVersion(boost::optional<DatabaseVersion> version,
                              uint64_t disambiguatingSequenceNum,
                              uint64_t forcedRefreshSequenceNum)
        : _dbVersion(std::move(version)),
          _disambiguatingSequenceNum(disambiguatingSequenceNum),
          _forcedRefreshSequenceNum(forcedRefreshSequenceNum) {}

    void setDatabaseVersion(const DatabaseVersion& version);

    boost::optional<DatabaseVersion> _dbVersion;

    uint64_t _disambiguatingSequenceNum{0};
    uint64_t _forcedRefreshSequenceNum{0};
};

/**
 * This is the root of the "read-only" hierarchy of cached catalog metadata. It is read only
 * in the sense that it only reads from the persistent store, but never writes to it. Instead
 * writes happen through the ShardingCatalogManager and the cache hierarchy needs to be invalidated.
 */
class CatalogCache {
    CatalogCache(const CatalogCache&) = delete;
    CatalogCache& operator=(const CatalogCache&) = delete;

public:
    CatalogCache(ServiceContext* service, CatalogCacheLoader& cacheLoader);
    virtual ~CatalogCache();

    /**
     * Shuts down and joins the executor used by all the caches to run their blocking work.
     */
    void shutDownAndJoin();

    /**
     * Blocking method that ensures the specified database is in the cache, loading it if necessary,
     * and returns it. If the database was not in cache, all the sharded collections will be in the
     * 'needsRefresh' state.
     */
    virtual StatusWith<CachedDatabaseInfo> getDatabase(OperationContext* opCtx,
                                                       const DatabaseName& dbName);

    /**
     * Blocking method to get both the placement information and the index information for a
     * collection.
     *
     * If the collection is sharded, returns placement info initialized with a ChunkManager and a
     * list of global indexes that may be empty if no global indexes exist. If the collection is not
     * sharded, returns placement info initialized with the primary shard for the specified database
     * and an empty list representing no global indexes. If an error occurs while loading the
     * metadata, returns a failed status.
     *
     * If the given atClusterTime is so far in the past that it is not possible to construct
     * placement info, returns a StaleClusterTime error.
     */
    StatusWith<CollectionRoutingInfo> getCollectionRoutingInfoAt(OperationContext* opCtx,
                                                                 const NamespaceString& nss,
                                                                 Timestamp atClusterTime);

    /**
     * Same as the getCollectionRoutingInfoAt call above, but returns the latest known routing
     * information for the specified namespace.
     *
     * While this method may fail under the same circumstances as getCollectionRoutingInfoAt, it is
     * guaranteed to never throw StaleClusterTime, because the latest routing information should
     * always be available.
     */
    virtual StatusWith<CollectionRoutingInfo> getCollectionRoutingInfo(OperationContext* opCtx,
                                                                       const NamespaceString& nss,
                                                                       bool allowLocks = false);

    /**
     * Same as getDatbase above, but in addition forces the database entry to be refreshed.
     */
    StatusWith<CachedDatabaseInfo> getDatabaseWithRefresh(OperationContext* opCtx,
                                                          const DatabaseName& dbName);

    /**
     * Same as getCollectionRoutingInfo above, but in addition causes the namespace to be refreshed.
     */
    StatusWith<CollectionRoutingInfo> getCollectionRoutingInfoWithRefresh(
        OperationContext* opCtx, const NamespaceString& nss);

    /**
     * Same as getCollectionRoutingInfo above, but in addition, causes the placement information for
     * the namespace to be refreshed. Will only refresh the index information if the collection
     * uuid from the placement information does not match the collection uuid from the cached index
     * information.
     */
    StatusWith<CollectionRoutingInfo> getCollectionRoutingInfoWithPlacementRefresh(
        OperationContext* opCtx, const NamespaceString& nss);

    /**
     * Same as getCollectionRoutingInfo above, but in addition, causes the index information for the
     * namespace to be refreshed. Will only refresh the placement information if the collection uuid
     * from the index information does not match the collection uuid from the cached placement
     * information.
     */
    StatusWith<CollectionRoutingInfo> getCollectionRoutingInfoWithIndexRefresh(
        OperationContext* opCtx, const NamespaceString& nss);

    /**
     * Same as getCollectionRoutingInfo above, but throws NamespaceNotSharded error if the namespace
     * is not sharded.
     */
    CollectionRoutingInfo getShardedCollectionRoutingInfo(OperationContext* opCtx,
                                                          const NamespaceString& nss);

    /**
     * Same as getCollectionRoutingInfoWithRefresh above, but in addition returns a
     * NamespaceNotSharded error if the collection is not sharded.
     */
    StatusWith<CollectionRoutingInfo> getShardedCollectionRoutingInfoWithRefresh(
        OperationContext* opCtx, const NamespaceString& nss);

    /**
     * Same as getCollectionRoutingInfoWithPlacementRefresh above, but in addition returns a
     * NamespaceNotSharded error if the collection is not sharded.
     */
    StatusWith<CollectionRoutingInfo> getShardedCollectionRoutingInfoWithPlacementRefresh(
        OperationContext* opCtx, const NamespaceString& nss);


    /**
     * Same as getCollectionRoutingInfo above, but throws NamespaceNotFound error if the namespace
     * is not tracked.
     */
    CollectionRoutingInfo getTrackedCollectionRoutingInfo(OperationContext* opCtx,
                                                          const NamespaceString& nss);

    /**
     * Same as getCollectionRoutingInfoWithRefresh above, but in addition returns a
     * NamespaceNotFound error if the collection is not tracked.
     */
    StatusWith<CollectionRoutingInfo> getTrackedCollectionRoutingInfoWithRefresh(
        OperationContext* opCtx, const NamespaceString& nss);

    /**
     * Same as getCollectionRoutingInfoWithPlacementRefresh above, but in addition returns a
     * NamespaceNotFound error if the collection is not tracked.
     */
    StatusWith<CollectionRoutingInfo> getTrackedCollectionRoutingInfoWithPlacementRefresh(
        OperationContext* opCtx, const NamespaceString& nss);

    /**
     * Advances the version in the cache for the given database.
     *
     * To be called with the wantedVersion returned by a targeted node in case of a
     * StaleDatabaseVersion response.
     *
     * In the case the passed version is boost::none, invalidates the cache for the given database.
     */
    void onStaleDatabaseVersion(const DatabaseName& dbName,
                                const boost::optional<DatabaseVersion>& wantedVersion);

    /**
     * Invalidates a single shard for the current collection if the epochs given in the chunk
     * versions match. Otherwise, invalidates the entire collection, causing any future targetting
     * requests to block on an upcoming catalog cache refresh.
     */
    void invalidateShardOrEntireCollectionEntryForShardedCollection(
        const NamespaceString& nss,
        const boost::optional<ShardVersion>& wantedVersion,
        const ShardId& shardId);

    /**
     * Notifies the cache that there is a (possibly) newer collection version on the backing store.
     */
    virtual void advanceCollectionTimeInStore(const NamespaceString& nss,
                                              const ChunkVersion& newVersionInStore);

    /**
     * Non-blocking method, which invalidates all namespaces which contain data on the specified
     * shard and all databases which have the shard listed as their primary shard.
     */
    void invalidateEntriesThatReferenceShard(const ShardId& shardId);

    /**
     * Non-blocking method, which removes the entire specified database (including its collections)
     * from the cache.
     */
    void purgeDatabase(const DatabaseName& dbName);

    /**
     * Non-blocking method, which removes all databases (including their collections) from the
     * cache.
     */
    void purgeAllDatabases();

    /**
     * Reports statistics about the catalog cache to be used by serverStatus
     */
    void report(BSONObjBuilder* builder) const;

    /**
     * Non-blocking method that marks the current database entry for the dbName as needing
     * refresh. Will cause all further targetting attempts to block on a catalog cache refresh,
     * even if they do not require causal consistency.
     */
    void invalidateDatabaseEntry_LINEARIZABLE(const DatabaseName& dbName);

    /**
     * Non-blocking method that marks the current collection entry for the namespace as needing
     * refresh. Will cause all further targetting attempts to block on a catalog cache refresh,
     * even if they do not require causal consistency.
     */
    void invalidateCollectionEntry_LINEARIZABLE(const NamespaceString& nss);

    void invalidateIndexEntry_LINEARIZABLE(const NamespaceString& nss);

    /**
     * Peeks at the collection routing cache and returns the currently cached collection version.
     * Returns boost::none if none is cached. Never blocks waiting for a refresh.
     */
    boost::optional<ChunkVersion> peekCollectionCacheVersion(const NamespaceString& nss);

private:
    class DatabaseCache : public DatabaseTypeCache {
    public:
        DatabaseCache(ServiceContext* service,
                      ThreadPoolInterface& threadPool,
                      CatalogCacheLoader& catalogCacheLoader);

    private:
        LookupResult _lookupDatabase(OperationContext* opCtx,
                                     const DatabaseName& dbName,
                                     const ValueHandle& dbType,
                                     const ComparableDatabaseVersion& previousDbVersion);

        CatalogCacheLoader& _catalogCacheLoader;
        Mutex _mutex = MONGO_MAKE_LATCH("DatabaseCache::_mutex");
    };

    class CollectionCache : public RoutingTableHistoryCache {
    public:
        CollectionCache(ServiceContext* service,
                        ThreadPoolInterface& threadPool,
                        CatalogCacheLoader& catalogCacheLoader);

        void reportStats(BSONObjBuilder* builder) const;

    private:
        LookupResult _lookupCollection(OperationContext* opCtx,
                                       const NamespaceString& nss,
                                       const ValueHandle& collectionHistory,
                                       const ComparableChunkVersion& previousChunkVersion);

        CatalogCacheLoader& _catalogCacheLoader;
        Mutex _mutex = MONGO_MAKE_LATCH("CollectionCache::_mutex");

        struct Stats {
            // Tracks how many incremental refreshes are waiting to complete currently
            AtomicWord<long long> numActiveIncrementalRefreshes{0};

            // Cumulative, always-increasing counter of how many incremental refreshes have been
            // kicked off
            AtomicWord<long long> countIncrementalRefreshesStarted{0};

            // Tracks how many full refreshes are waiting to complete currently
            AtomicWord<long long> numActiveFullRefreshes{0};

            // Cumulative, always-increasing counter of how many full refreshes have been kicked off
            AtomicWord<long long> countFullRefreshesStarted{0};

            // Cumulative, always-increasing counter of how many full or incremental refreshes
            // failed for whatever reason
            AtomicWord<long long> countFailedRefreshes{0};

            /**
             * Reports the accumulated statistics for serverStatus.
             */
            void report(BSONObjBuilder* builder) const;

        } _stats;

        void _updateRefreshesStats(bool isIncremental, bool add);
    };

    class IndexCache : public ShardingIndexesCatalogRTCBase {
    public:
        IndexCache(ServiceContext* service, ThreadPoolInterface& threadPool);

    private:
        LookupResult _lookupIndexes(OperationContext* opCtx,
                                    const NamespaceString& nss,
                                    const ValueHandle& indexes,
                                    const ComparableIndexVersion& previousIndexVersion);
        Mutex _mutex = MONGO_MAKE_LATCH("IndexCache::_mutex");
    };

    // Callers of this internal function that are passing allowLocks must handle allowLocks failures
    // by checking for ErrorCodes::ShardCannotRefreshDueToLocksHeld and addint the full namespace to
    // the exception.
    StatusWith<CachedDatabaseInfo> _getDatabase(OperationContext* opCtx,
                                                const DatabaseName& dbName,
                                                bool allowLocks = false);

    StatusWith<ChunkManager> _getCollectionPlacementInfoAt(OperationContext* opCtx,
                                                           const NamespaceString& nss,
                                                           boost::optional<Timestamp> atClusterTime,
                                                           bool allowLocks = false);

    boost::optional<ShardingIndexesCatalogCache> _getCollectionIndexInfoAt(
        OperationContext* opCtx, const NamespaceString& nss, bool allowLocks = false);

    void _triggerPlacementVersionRefresh(const NamespaceString& nss);

    void _triggerIndexVersionRefresh(const NamespaceString& nss);

    // Same as getCollectionRoutingInfo but will fetch the index information from the cache even if
    // the placement information is not sharded. Used internally when the a refresh is requested for
    // the index component.
    StatusWith<CollectionRoutingInfo> _getCollectionRoutingInfoWithoutOptimization(
        OperationContext* opCtx, const NamespaceString& nss);

    StatusWith<CollectionRoutingInfo> _retryUntilConsistentRoutingInfo(
        OperationContext* opCtx,
        const NamespaceString& nss,
        ChunkManager&& cm,
        boost::optional<ShardingIndexesCatalogCache>&& sii);

    // Interface from which chunks will be retrieved
    CatalogCacheLoader& _cacheLoader;

    // Executor on which the caches below will execute their blocking work
    ThreadPool _executor;

    DatabaseCache _databaseCache;

    CollectionCache _collectionCache;

    IndexCache _indexCache;

    /**
     * Encapsulates runtime statistics across all databases and collections in this catalog cache
     */
    struct Stats {
        // Counts how many times threads hit stale config exception (which is what triggers metadata
        // refreshes)
        AtomicWord<long long> countStaleConfigErrors{0};

        // Cumulative, always-increasing counter of how much time threads waiting for refresh
        // combined
        AtomicWord<long long> totalRefreshWaitTimeMicros{0};

        /**
         * Reports the accumulated statistics for serverStatus.
         */
        void report(BSONObjBuilder* builder) const;

    } _stats;
};

}  // namespace mongo
