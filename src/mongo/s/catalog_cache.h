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

#include "mongo/base/string_data.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog_cache_loader.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/shard.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/read_through_cache.h"
#include "mongo/util/string_map.h"

namespace mongo {

class BSONObjBuilder;

static constexpr int kMaxNumStaleVersionRetries = 10;

/**
 * If true, this operation should block behind a catalog cache refresh. Otherwise, the operation
 * will block skip the catalog cache refresh.
 */
extern const OperationContext::Decoration<bool> operationShouldBlockBehindCatalogCacheRefresh;

/**
 * Constructed exclusively by the CatalogCache to be used as vector clock (Time) to drive
 * DatabaseCache's refreshes.
 *
 * The DatabaseVersion class contains a UUID that is not comparable,
 * in fact is impossible to compare two different DatabaseVersion that have different UUIDs.
 *
 * This class wrap a DatabaseVersion object to make it always comparable by timestamping it with a
 * node-local sequence number (_uuidDisambiguatingSequenceNum).
 *
 * This class class should go away once a cluster-wide comparable DatabaseVersion will be
 * implemented.
 */
class ComparableDatabaseVersion {
public:
    /**
     * Creates a ComparableDatabaseVersion that wraps the given DatabaseVersion.
     * Each object created through this method will have a local sequence number greater than the
     * previously created ones.
     */
    static ComparableDatabaseVersion makeComparableDatabaseVersion(const DatabaseVersion& version);

    /**
     * Empty constructor needed by the ReadThroughCache.
     *
     * Instances created through this constructor will be always less then the ones created through
     * the static constructor.
     */
    ComparableDatabaseVersion() = default;

    const DatabaseVersion& getVersion() const {
        return *_dbVersion;
    }

    std::string toString() const;

    bool sameUuid(const ComparableDatabaseVersion& other) const {
        return _dbVersion->getUuid() == other._dbVersion->getUuid();
    }

    bool operator==(const ComparableDatabaseVersion& other) const;

    bool operator!=(const ComparableDatabaseVersion& other) const {
        return !(*this == other);
    }

    /**
     * In case the two compared instances have different UUIDs, the most recently created one will
     * be greater, otherwise the comparison will be driven by the lastMod field of the underlying
     * DatabaseVersion.
     */
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
    static AtomicWord<uint64_t> _uuidDisambiguatingSequenceNumSource;

    ComparableDatabaseVersion(const DatabaseVersion& version,
                              uint64_t uuidDisambiguatingSequenceNum)
        : _dbVersion(version), _uuidDisambiguatingSequenceNum(uuidDisambiguatingSequenceNum) {}

    boost::optional<DatabaseVersion> _dbVersion;

    // Locally incremented sequence number that allows to compare two database versions with
    // different UUIDs. Each new comparableDatabaseVersion will have a greater sequence number then
    // the ones created before.
    uint64_t _uuidDisambiguatingSequenceNum{0};
};

/**
 * Constructed exclusively by the CatalogCache, contains a reference to the cached information for
 * the specified database.
 */
class CachedDatabaseInfo {
public:
    const ShardId& primaryId() const;
    std::shared_ptr<Shard> primary() const {
        return _primaryShard;
    };

    bool shardingEnabled() const;
    DatabaseVersion databaseVersion() const;

private:
    friend class CatalogCache;

    CachedDatabaseInfo(DatabaseType dbt, std::shared_ptr<Shard> primaryShard);

    DatabaseType _dbt;
    std::shared_ptr<Shard> _primaryShard;
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
    ~CatalogCache();

    /**
     * Blocking method that ensures the specified database is in the cache, loading it if necessary,
     * and returns it. If the database was not in cache, all the sharded collections will be in the
     * 'needsRefresh' state.
     */
    StatusWith<CachedDatabaseInfo> getDatabase(OperationContext* opCtx,
                                               StringData dbName,
                                               bool allowLocks = false);

    /**
     * Blocking method to get the routing information for a specific collection at a given cluster
     * time.
     *
     * If the collection is sharded, returns routing info initialized with a ChunkManager. If the
     * collection is not sharded, returns routing info initialized with the primary shard for the
     * specified database. If an error occurs while loading the metadata, returns a failed status.
     *
     * If the given atClusterTime is so far in the past that it is not possible to construct routing
     * info, returns a StaleClusterTime error.
     */
    StatusWith<ChunkManager> getCollectionRoutingInfoAt(OperationContext* opCtx,
                                                        const NamespaceString& nss,
                                                        Timestamp atClusterTime);

    /**
     * Same as the getCollectionRoutingInfoAt call above, but returns the latest known routing
     * information for the specified namespace.
     *
     * While this method may fail under the same circumstances as getCollectionRoutingInfoAt, it is
     * guaranteed to never return StaleClusterTime, because the latest routing information should
     * always be available.
     */
    StatusWith<ChunkManager> getCollectionRoutingInfo(OperationContext* opCtx,
                                                      const NamespaceString& nss,
                                                      bool allowLocks = false);

    /**
     * Same as getDatbase above, but in addition forces the database entry to be refreshed.
     */
    StatusWith<CachedDatabaseInfo> getDatabaseWithRefresh(OperationContext* opCtx,
                                                          StringData dbName);

    /**
     * Same as getCollectionRoutingInfo above, but in addition causes the namespace to be refreshed.
     */
    StatusWith<ChunkManager> getCollectionRoutingInfoWithRefresh(OperationContext* opCtx,
                                                                 const NamespaceString& nss);

    /**
     * Same as getCollectionRoutingInfoWithRefresh above, but in addition returns a
     * NamespaceNotSharded error if the collection is not sharded.
     */
    StatusWith<ChunkManager> getShardedCollectionRoutingInfoWithRefresh(OperationContext* opCtx,
                                                                        const NamespaceString& nss);

    /**
     * Advances the version in the cache for the given database.
     *
     * To be called with the wantedVersion returned by a targeted node in case of a
     * StaleDatabaseVersion response.
     *
     * In the case the passed version is boost::none, nothing will be done.
     */
    void onStaleDatabaseVersion(const StringData dbName,
                                const boost::optional<DatabaseVersion>& wantedVersion);

    /**
     * Sets whether this operation should block behind a catalog cache refresh.
     */
    static void setOperationShouldBlockBehindCatalogCacheRefresh(OperationContext* opCtx,
                                                                 bool shouldBlock);

    /**
     * Invalidates a single shard for the current collection if the epochs given in the chunk
     * versions match. Otherwise, invalidates the entire collection, causing any future targetting
     * requests to block on an upcoming catalog cache refresh.
     */
    void invalidateShardOrEntireCollectionEntryForShardedCollection(
        const NamespaceString& nss,
        const boost::optional<ChunkVersion>& wantedVersion,
        const ShardId& shardId);

    /**
     * Throws a StaleConfigException if this catalog cache does not have an entry for the given
     * namespace, or if the entry for the given namespace does not have the same epoch as
     * 'targetCollectionVersion'. Does not perform any refresh logic. Ignores everything except the
     * epoch of 'targetCollectionVersion' when performing the check, but needs the entire target
     * version to throw a StaleConfigException.
     */
    void checkEpochOrThrow(const NamespaceString& nss,
                           const ChunkVersion& targetCollectionVersion,
                           const ShardId& shardId);

    /**
     * Non-blocking method, which invalidates all namespaces which contain data on the specified
     * shard and all databases which have the shard listed as their primary shard.
     */
    void invalidateEntriesThatReferenceShard(const ShardId& shardId);

    /**
     * Non-blocking method, which removes the entire specified database (including its collections)
     * from the cache.
     */
    void purgeDatabase(StringData dbName);

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
     * Checks if the current operation was ever marked as needing refresh. If the curent operation
     * was marked as needing refresh, updates the relevant counters inside the Stats struct.
     */
    void checkAndRecordOperationBlockedByRefresh(OperationContext* opCtx, mongo::LogicalOp opType);

    /**
     * Non-blocking method that marks the current collection entry for the namespace as needing
     * refresh. Will cause all further targetting attempts to block on a catalog cache refresh,
     * even if they do not require causal consistency.
     */
    void invalidateCollectionEntry_LINEARIZABLE(const NamespaceString& nss);

private:
    // Make the cache entries friends so they can access the private classes below
    friend class CachedDatabaseInfo;

    class DatabaseCache
        : public ReadThroughCache<std::string, DatabaseType, ComparableDatabaseVersion> {
    public:
        DatabaseCache(ServiceContext* service,
                      ThreadPoolInterface& threadPool,
                      CatalogCacheLoader& catalogCacheLoader);

    private:
        LookupResult _lookupDatabase(OperationContext* opCtx,
                                     const std::string& dbName,
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

        void _updateRefreshesStats(const bool isIncremental, const bool add);
    };

    StatusWith<ChunkManager> _getCollectionRoutingInfoAt(OperationContext* opCtx,
                                                         const NamespaceString& nss,
                                                         boost::optional<Timestamp> atClusterTime,
                                                         bool allowLocks = false);

    // Interface from which chunks will be retrieved
    CatalogCacheLoader& _cacheLoader;

    // Encapsulates runtime statistics across all collections in the catalog cache
    struct Stats {
        // Counts how many times threads hit stale config exception (which is what triggers metadata
        // refreshes)
        AtomicWord<long long> countStaleConfigErrors{0};

        // Cumulative, always-increasing counter of how much time threads waiting for refresh
        // combined
        AtomicWord<long long> totalRefreshWaitTimeMicros{0};

        // Cumulative, always-increasing counter of how many operations have been blocked by a
        // catalog cache refresh. Broken down by operation type to match the operations tracked
        // by the OpCounters class.
        struct OperationsBlockedByRefresh {
            AtomicWord<long long> countAllOperations{0};
            AtomicWord<long long> countInserts{0};
            AtomicWord<long long> countQueries{0};
            AtomicWord<long long> countUpdates{0};
            AtomicWord<long long> countDeletes{0};
            AtomicWord<long long> countCommands{0};
        } operationsBlockedByRefresh;

        /**
         * Reports the accumulated statistics for serverStatus.
         */
        void report(BSONObjBuilder* builder) const;

    } _stats;

    std::shared_ptr<ThreadPool> _executor;

    DatabaseCache _databaseCache;

    CollectionCache _collectionCache;
};

}  // namespace mongo
