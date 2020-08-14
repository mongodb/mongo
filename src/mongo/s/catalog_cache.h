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

#include <memory>

#include "mongo/base/string_data.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/s/catalog/type_database.h"
#include "mongo/s/catalog_cache_loader.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard.h"
#include "mongo/s/database_version_gen.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/read_through_cache.h"
#include "mongo/util/string_map.h"

namespace mongo {

class BSONObjBuilder;
class CachedDatabaseInfo;
class CachedCollectionRoutingInfo;
class OperationContext;

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
 * node-local sequence number (_dbVersionLocalSequence).
 *
 * This class class should go away once a cluster-wide comparable DatabaseVersion will be
 * implemented.
 */
class ComparableDatabaseVersion {
public:
    /*
     * Create a ComparableDatabaseVersion that wraps the given DatabaseVersion.
     * Each object created through this method will have a local sequence number grater then the
     * previously created ones.
     */
    static ComparableDatabaseVersion makeComparableDatabaseVersion(const DatabaseVersion& version);

    /*
     * Empty constructor needed by the ReadThroughCache.
     *
     * Instances created through this constructor will be always less then the ones created through
     * the static constructor.
     */
    ComparableDatabaseVersion() = default;

    const DatabaseVersion& getVersion() const;

    uint64_t getLocalSequenceNum() const;

    BSONObj toBSON() const;

    std::string toString() const;

    // Rerturns true if the two versions have the same UUID
    bool sameUuid(const ComparableDatabaseVersion& other) const {
        return _dbVersion.getUuid() == other._dbVersion.getUuid();
    }

    bool operator==(const ComparableDatabaseVersion& other) const {
        return sameUuid(other) && (_dbVersion.getLastMod() == other._dbVersion.getLastMod());
    }

    bool operator!=(const ComparableDatabaseVersion& other) const {
        return !(*this == other);
    }

    /*
     * In the case the two compared instances have different UUIDs the most recently created one
     * will be grater, otherwise the comparision will be driven by the lastMod field of the
     * underlying DatabaseVersion.
     */
    bool operator<(const ComparableDatabaseVersion& other) const {
        if (sameUuid(other)) {
            return _dbVersion.getLastMod() < other._dbVersion.getLastMod();
        } else {
            return _localSequenceNum < other._localSequenceNum;
        }
    }

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
    static AtomicWord<uint64_t> _localSequenceNumSource;

    ComparableDatabaseVersion(const DatabaseVersion& version, uint64_t localSequenceNum)
        : _dbVersion(version), _localSequenceNum(localSequenceNum) {}

    DatabaseVersion _dbVersion;
    // Locally incremented sequence number that allows to compare two database versions with
    // different UUIDs. Each new comparableDatabaseVersion will have a greater sequence number then
    // the ones created before.
    uint64_t _localSequenceNum{0};
};

/**
 * Constructed to be used exclusively by the CatalogCache as a vector clock (Time) to drive
 * CollectionCache's lookups.
 *
 * The ChunkVersion class contains an non comparable epoch, which makes impossible to compare two
 * ChunkVersions when their epochs's differ.
 *
 * This class wraps a ChunkVersion object with a node-local sequence number (_localSequenceNum) that
 * allows the comparision.
 *
 * This class should go away once a cluster-wide comparable ChunkVersion is implemented.
 */
class ComparableChunkVersion {
public:
    static ComparableChunkVersion makeComparableChunkVersion(const ChunkVersion& version);

    ComparableChunkVersion() = default;

    const ChunkVersion& getVersion() const;

    uint64_t getLocalSequenceNum() const;

    BSONObj toBSON() const;

    std::string toString() const;

    bool sameEpoch(const ComparableChunkVersion& other) const {
        return _chunkVersion.epoch() == other._chunkVersion.epoch();
    }

    bool operator==(const ComparableChunkVersion& other) const {
        return sameEpoch(other) &&
            (_chunkVersion.majorVersion() == other._chunkVersion.majorVersion() &&
             _chunkVersion.minorVersion() == other._chunkVersion.minorVersion());
    }

    bool operator!=(const ComparableChunkVersion& other) const {
        return !(*this == other);
    }

    bool operator<(const ComparableChunkVersion& other) const {
        if (sameEpoch(other)) {
            return _chunkVersion.majorVersion() < other._chunkVersion.majorVersion() ||
                (_chunkVersion.majorVersion() == other._chunkVersion.majorVersion() &&
                 _chunkVersion.minorVersion() < other._chunkVersion.minorVersion());
        } else {
            return _localSequenceNum < other._localSequenceNum;
        }
    }

    bool operator>(const ComparableChunkVersion& other) const {
        return other < *this;
    }

    bool operator<=(const ComparableChunkVersion& other) const {
        return !(*this > other);
    }

    bool operator>=(const ComparableChunkVersion& other) const {
        return !(*this < other);
    }

private:
    static AtomicWord<uint64_t> _localSequenceNumSource;

    ComparableChunkVersion(const ChunkVersion& version, uint64_t localSequenceNum)
        : _chunkVersion(version), _localSequenceNum(localSequenceNum) {}

    ChunkVersion _chunkVersion;

    // Locally incremented sequence number that allows to compare two colection versions with
    // different epochs. Each new comparableChunkVersion will have a greater sequence number than
    // the ones created before.
    uint64_t _localSequenceNum{0};
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
 * Constructed exclusively by the CatalogCache.
 *
 * This RoutingInfo can be considered a "package" of routing info for the database and for the
 * collection. Once unsharded collections are treated as sharded collections with a single chunk,
 * they will also have a ChunkManager with a "chunk distribution." At that point, this "package" can
 * be dismantled: routing for commands that route by database can directly retrieve the
 * CachedDatabaseInfo, while routing for commands that route by collection can directly retrieve the
 * ChunkManager.
 */
class CachedCollectionRoutingInfo {
public:
    CachedDatabaseInfo db() const {
        return _db;
    }

    const ChunkManager* cm() const {
        return _cm.get_ptr();
    }

private:
    friend class CatalogCache;
    friend class CachedDatabaseInfo;

    CachedCollectionRoutingInfo(NamespaceString nss,
                                CachedDatabaseInfo db,
                                boost::optional<ChunkManager> cm);

    NamespaceString _nss;

    // Copy of the database's cached info.
    CachedDatabaseInfo _db;

    // Shared reference to the collection's cached chunk distribution if sharded, otherwise
    // boost::none. This is a shared reference rather than a copy because the chunk distribution can
    // be large.
    boost::optional<ChunkManager> _cm;
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
    StatusWith<CachedDatabaseInfo> getDatabase(OperationContext* opCtx, StringData dbName);

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
    StatusWith<CachedCollectionRoutingInfo> getCollectionRoutingInfoAt(OperationContext* opCtx,
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
    StatusWith<CachedCollectionRoutingInfo> getCollectionRoutingInfo(OperationContext* opCtx,
                                                                     const NamespaceString& nss);

    /**
     * Same as getDatbase above, but in addition forces the database entry to be refreshed.
     */
    StatusWith<CachedDatabaseInfo> getDatabaseWithRefresh(OperationContext* opCtx,
                                                          StringData dbName);

    /**
     * Same as getCollectionRoutingInfo above, but in addition causes the namespace to be refreshed.
     *
     * When forceRefreshFromThisThread is false, it's possible for this call to
     * join an ongoing refresh from another thread forceRefreshFromThisThread.
     * forceRefreshFromThisThread checks whether it joined another thread and
     * then forces it to try again, which is necessary in cases where calls to
     * getCollectionRoutingInfoWithRefresh must be causally consistent
     *
     * TODO: Remove this parameter in favor of using collection creation time +
     * collection version to decide when a refresh is necessary and provide
     * proper causal consistency
     */
    StatusWith<CachedCollectionRoutingInfo> getCollectionRoutingInfoWithRefresh(
        OperationContext* opCtx,
        const NamespaceString& nss,
        bool forceRefreshFromThisThread = false);

    /**
     * Same as getCollectionRoutingInfoWithRefresh above, but in addition returns a
     * NamespaceNotSharded error if the collection is not sharded.
     */
    StatusWith<CachedCollectionRoutingInfo> getShardedCollectionRoutingInfoWithRefresh(
        OperationContext* opCtx, const NamespaceString& nss);

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
     * Non-blocking method that marks the current cached collection entry as needing refresh if its
     * collectionVersion matches the input's ChunkManager's collectionVersion.
     *
     * To be called if using the input routing info caused a StaleShardVersion to be received.
     */
    void onStaleShardVersion(CachedCollectionRoutingInfo&&, const ShardId& staleShardId);

    /**
     * Gets whether this operation should block behind a catalog cache refresh.
     */
    static bool getOperationShouldBlockBehindCatalogCacheRefresh(OperationContext* opCtx);

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
        OperationContext* opCtx,
        const NamespaceString& nss,
        boost::optional<ChunkVersion> wantedVersion,
        const ChunkVersion& receivedVersion,
        ShardId shardId);

    /**
     * Non-blocking method that marks the current collection entry for the namespace as needing
     * refresh due to an epoch change. Will cause all further targetting attempts for this
     * namespace to block on a catalog cache refresh.
     */
    void onEpochChange(const NamespaceString& nss);

    /**
     * Throws a StaleConfigException if this catalog cache does not have an entry for the given
     * namespace, or if the entry for the given namespace does not have the same epoch as
     * 'targetCollectionVersion'. Does not perform any refresh logic. Ignores everything except the
     * epoch of 'targetCollectionVersion' when performing the check, but needs the entire target
     * version to throw a StaleConfigException.
     */
    void checkEpochOrThrow(const NamespaceString& nss,
                           ChunkVersion targetCollectionVersion,
                           const ShardId& shardId) const;

    /**
     * Non-blocking method, which invalidates the shard for the routing table for the specified
     * namespace. If that shard is targetted in the future, getCollectionRoutingInfo will wait on a
     * refresh.
     */
    void invalidateShardForShardedCollection(const NamespaceString& nss,
                                             const ShardId& staleShardId);

    /**
     * Non-blocking method, which invalidates all namespaces which contain data on the specified
     * shard and all databases which have the shard listed as their primary shard.
     */
    void invalidateEntriesThatReferenceShard(const ShardId& shardId);

    /**
     * Non-blocking method, which removes the entire specified collection from the cache (resulting
     * in full refresh on subsequent access)
     */
    void purgeCollection(const NamespaceString& nss);

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

private:
    // Make the cache entries friends so they can access the private classes below
    friend class CachedDatabaseInfo;
    friend class CachedCollectionRoutingInfo;

    /**
     * Cache entry describing a collection.
     */
    struct CollectionRoutingInfoEntry {
        CollectionRoutingInfoEntry() = default;
        // Disable copy (and move) semantics
        CollectionRoutingInfoEntry(const CollectionRoutingInfoEntry&) = delete;
        CollectionRoutingInfoEntry& operator=(const CollectionRoutingInfoEntry&) = delete;

        // Specifies whether this cache entry needs a refresh (in which case routingInfo should not
        // be relied on) or it doesn't, in which case there should be a non-null routingInfo.
        bool needsRefresh{true};

        // Specifies whether the namespace has had an epoch change, which indicates that every
        // shard should block on an upcoming refresh.
        bool epochHasChanged{true};

        // Contains a notification to be waited on for the refresh to complete (only available if
        // needsRefresh is true)
        std::shared_ptr<Notification<Status>> refreshCompletionNotification;

        // Contains the cached routing information (only available if needsRefresh is false)
        std::shared_ptr<RoutingTableHistory> routingInfo;
    };

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

    /**
     * Non-blocking call which schedules an asynchronous refresh for the specified namespace. The
     * namespace must be in the 'needRefresh' state.
     */
    void _scheduleCollectionRefresh(WithLock,
                                    ServiceContext* service,
                                    std::shared_ptr<CollectionRoutingInfoEntry> collEntry,
                                    NamespaceString const& nss,
                                    int refreshAttempt);

    /**
     * Marks a collection entry as needing refresh. Will create the collection entry if one does
     * not exist. Also marks the epoch as changed, which will cause all further targetting requests
     * against this namespace to block upon a catalog cache refresh.
     */
    void _createOrGetCollectionEntryAndMarkEpochStale(const NamespaceString& nss);

    /**
     * Marks a collection entry as needing refresh. Will create the collection entry if one does
     * not exist. Will mark the given shard ID as stale, which will cause all further targetting
     * requests for the given shard for this namespace to block upon a catalog cache refresh.
     */
    void _createOrGetCollectionEntryAndMarkShardStale(const NamespaceString& nss,
                                                      const ShardId& shardId);

    /**
     * Marks a collection entry as needing refresh. Will create the collection entry if one does
     * not exist.
     */
    void _createOrGetCollectionEntryAndMarkAsNeedsRefresh(const NamespaceString& nss);

    /**
     * Retrieves the collection entry for the given namespace, creating the entry if one does not
     * already exist.
     */
    std::shared_ptr<CollectionRoutingInfoEntry> _createOrGetCollectionEntry(
        WithLock wl, const NamespaceString& nss);

    /**
     * Used as a flag to indicate whether or not this thread performed its own
     * refresh for certain helper functions
     *
     * kPerformedRefresh is used only when the calling thread performed the
     * refresh *itself*
     *
     * kDidNotPerformRefresh is used either when there was an error or when
     * this thread joined an ongoing refresh
     */
    enum class RefreshAction {
        kPerformedRefresh,
        kDidNotPerformRefresh,
    };

    /**
     * Return type for helper functions performing refreshes so that they can
     * indicate both status and whether or not this thread performed its own
     * refresh
     */
    struct RefreshResult {
        // Status containing result of refresh
        StatusWith<CachedCollectionRoutingInfo> statusWithInfo;
        RefreshAction actionTaken;
    };

    /**
     * Retrieves the collection routing info for this namespace after blocking on a catalog cache
     * refresh.
     */
    CatalogCache::RefreshResult _getCollectionRoutingInfoWithForcedRefresh(
        OperationContext* opctx, const NamespaceString& nss);

    /**
     * Helper function used when we need the refresh action taken (e.g. when we
     * want to force refresh)
     */
    CatalogCache::RefreshResult _getCollectionRoutingInfo(OperationContext* opCtx,
                                                          const NamespaceString& nss);

    CatalogCache::RefreshResult _getCollectionRoutingInfoAt(
        OperationContext* opCtx,
        const NamespaceString& nss,
        boost::optional<Timestamp> atClusterTime);

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

        // Tracks how many incremental refreshes are waiting to complete currently
        AtomicWord<long long> numActiveIncrementalRefreshes{0};

        // Cumulative, always-increasing counter of how many incremental refreshes have been kicked
        // off
        AtomicWord<long long> countIncrementalRefreshesStarted{0};

        // Tracks how many full refreshes are waiting to complete currently
        AtomicWord<long long> numActiveFullRefreshes{0};

        // Cumulative, always-increasing counter of how many full refreshes have been kicked off
        AtomicWord<long long> countFullRefreshesStarted{0};

        // Cumulative, always-increasing counter of how many full or incremental refreshes failed
        // for whatever reason
        AtomicWord<long long> countFailedRefreshes{0};

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

    // Mutex to serialize access to the collection cache
    mutable Mutex _mutex = MONGO_MAKE_LATCH("CatalogCache::_mutex");
    // Map from full collection name to the routing info for that collection, grouped by database
    using CollectionInfoMap = StringMap<std::shared_ptr<CollectionRoutingInfoEntry>>;
    using CollectionsByDbMap = StringMap<CollectionInfoMap>;
    CollectionsByDbMap _collectionsByDb;
};

}  // namespace mongo
