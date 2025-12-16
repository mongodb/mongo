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

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/global_catalog/chunk_manager.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/router_role/routing_cache/catalog_cache_loader.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/concurrency/thread_pool_interface.h"
#include "mongo/util/modules.h"
#include "mongo/util/observable_mutex.h"
#include "mongo/util/read_through_cache.h"

#include <cstdint>
#include <string>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>

namespace mongo {

class ComparableDatabaseVersion;

using DatabaseTypeCache MONGO_MOD_PRIVATE = ReadThroughCache<DatabaseName,
                                                             DatabaseType,
                                                             ComparableDatabaseVersion,
                                                             ObservableMutex<stdx::mutex>>;
using DatabaseTypeValueHandle MONGO_MOD_USE_REPLACEMENT(CachedDatabaseInfo) =
    DatabaseTypeCache::ValueHandle;
using CachedDatabaseInfo MONGO_MOD_PUBLIC = DatabaseTypeValueHandle;

class MONGO_MOD_PUBLIC CollectionRoutingInfo {
public:
    CollectionRoutingInfo(CurrentChunkManager&& chunkManager, CachedDatabaseInfo&& dbInfo)
        : _dbInfo(std::move(dbInfo)), _cm(std::move(chunkManager)) {}

    CollectionRoutingInfo(PointInTimeChunkManager&& chunkManager, CachedDatabaseInfo&& dbInfo)
        : _dbInfo(std::move(dbInfo)), _cm(std::move(chunkManager)) {}

    /**
     * Variant type that can hold either a CurrentChunkManager or a PointInTimeChunkManager.
     *
     * This allows CollectionRoutingInfo to represent routing information in two different modes:
     * - CurrentChunkManager: Represents the latest known routing state for a collection
     * - PointInTimeChunkManager: Represents routing state at a specific point in time (cluster
     * time)
     *
     * Usage guidelines:
     * - Use std::visit() to access the held ChunkManager polymorphically when the operation works
     *   identically for both types
     * - Use std::get_if<CurrentChunkManager>() when you need to conditionally access features only
     *   available on CurrentChunkManager (e.g., getNShardsOwningChunks())
     * - Use std::holds_alternative<CurrentChunkManager>() to check which type is currently held
     *
     * Example:
     *   // Polymorphic access (works for both types):
     *   std::visit([](const auto& cm) { return cm.isSharded(); }, variant);
     *
     *   // Type-specific access:
     *   if (auto* currentCm = std::get_if<CurrentChunkManager>(&variant)) {
     *       currentCm->getAllShardIds(&shards);
     *   }
     *
     * TODO SERVER-114825: Investigate if it's possible to implement without this variant.
     */
    using ChunkManagerVariant = std::variant<CurrentChunkManager, PointInTimeChunkManager>;

    /**
     * Returns true if the collection is tracked in the global catalog.
     *
     * If this is the case the collection will also have an associated routing table (ChunkManager).
     */
    bool hasRoutingTable() const;

    /**
     * Returns true if the collection is tracked in the global catalog and is sharded.
     *
     * A sharded collection can have more than one chunks and chunks could be distributed on several
     * shards.
     */
    bool isSharded() const {
        return getChunkManager().isSharded();
    }

    /**
     * Returns a const reference to the ChunkManager held in this CollectionRoutingInfo.
     *
     * This method provides polymorphic access to common ChunkManager functionality that works
     * identically for both CurrentChunkManager and PointInTimeChunkManager.
     *
     * Use this method when:
     * - You need to call methods that are defined on the base ChunkManager class
     * - The operation doesn't require type-specific behavior
     * - You're working with shared functionality like:
     *   - isSharded()
     *   - isUnsplittable()
     *   - getShardKeyPattern()
     *   - getVersion()
     *   - forEachChunk() (respects point-in-time semantics automatically)
     *
     * Do NOT use this method when:
     * - You need to call CurrentChunkManager-specific methods like getAllShardIds() or
     *   getNShardsOwningChunks() - use getCurrentChunkManager() instead
     * - You need to know whether you have a current or point-in-time view - check the variant
     *   directly with std::holds_alternative<>()
     *
     * Example usage:
     *   const auto& cm = cri.getChunkManager();
     *   if (cm.isSharded()) {
     *       cm.forEachChunk([](const Chunk& chunk) { ... });
     *   }
     * @return A const reference to the base ChunkManager interface
     */
    const ChunkManager& getChunkManager() const {
        return std::visit([](const auto& cm) -> const ChunkManager& { return cm; }, _cm);
    }

    /**
     * Returns a const reference to the CurrentChunkManager held in this CollectionRoutingInfo.
     *
     * This method provides direct access to CurrentChunkManager-specific functionality that is
     * NOT available when using a PointInTimeChunkManager.
     *
     * Use this method when:
     * - You need to access the CURRENT (latest) state of the routing information
     * - You need to call methods that are ONLY available on CurrentChunkManager:
     *   - getAllShardIds(): Gets the exact current set of shards owning chunks
     *   - getNShardsOwningChunks(): Gets the exact current count of shards
     *   - getNextChunkOnShard(): Finds the next chunk on a specific shard
     * - You're performing operations that explicitly require non-point-in-time semantics:
     *   - Checking current cluster topology
     *   - Making routing decisions based on latest metadata
     *   - Administrative operations that need up-to-date information
     *
     * Do NOT use this method when:
     * - You're working with point-in-time reads (e.g., transactions with atClusterTime)
     * - You don't know whether the CollectionRoutingInfo contains a CurrentChunkManager or
     *   PointInTimeChunkManager - this will throw an exception if it's the wrong type
     * - You only need common ChunkManager functionality - use getChunkManager() instead
     *
     * Common usage patterns:
     *
     *   // Pattern 1: Direct access when you know it's current
     *   const auto& currentCm = cri.getCurrentChunkManager();
     *   currentCm.getAllShardIds(&allShards);
     *
     *   // Pattern 2: Conditional access (safer)
     *   if (std::holds_alternative<CurrentChunkManager>(cri._cm)) {
     *       const auto& currentCm = cri.getCurrentChunkManager();
     *       size_t nShards = currentCm.getNShardsOwningChunks();
     *   }
     *
     *   // Pattern 3: Using std::visit for type-specific behavior
     *   std::visit(OverloadedVisitor{
     *       [](const CurrentChunkManager& cm) {
     *           // Can call getCurrentChunkManager-only methods
     *           cm.getAllShardIds(&shards);
     *       },
     *       [](const PointInTimeChunkManager& cm) {
     *           // Different behavior for point-in-time
     *       }
     *   }, cri._cm);
     *
     * When this is created:
     * - getCollectionRoutingInfo() without atClusterTime returns CurrentChunkManager
     * - getCollectionRoutingInfoAt() with atClusterTime returns PointInTimeChunkManager
     * - getCollectionPlacementInfoWithRefresh() returns CurrentChunkManager
     *
     * @throws TAssertionException if this CollectionRoutingInfo contains a PointInTimeChunkManager
     *         instead of a CurrentChunkManager
     *
     * @return A const reference to the CurrentChunkManager
     */
    const CurrentChunkManager& getCurrentChunkManager() const {
        tassert(10271001,
                "Expected current ChunkManager but have PointInTimeChunkManager",
                std::holds_alternative<CurrentChunkManager>(_cm));
        return std::get<CurrentChunkManager>(_cm);
    }

    ShardVersion getCollectionVersion() const;
    ShardVersion getShardVersion(const ShardId& shardId) const;

    const CachedDatabaseInfo& getDatabaseInfo() const {
        return _dbInfo;
    }
    const ShardId& getDbPrimaryShardId() const;
    const DatabaseVersion& getDbVersion() const;
    // When set to true, the ShardVersions returned by this object will have the
    // 'ignoreCollectionUuidMismatch' flag set, thus instructing the receiving shards to ignore
    // collection UUID mismatches between the sharding catalog and their local catalog.
    bool shouldIgnoreUuidMismatch = false;


private:
    CachedDatabaseInfo _dbInfo;
    ChunkManagerVariant _cm;
};

/**
 * This class wrap a DatabaseVersion object augmenting it with:
 *  - a sequence number to allow for forced catalog cache refreshes
 *  - a sequence number to disambiguate scenarios in which the DatabaseVersion isn't valid
 */
class MONGO_MOD_PARENT_PRIVATE ComparableDatabaseVersion {
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
class MONGO_MOD_PUBLIC CatalogCache {
    CatalogCache(const CatalogCache&) = delete;
    CatalogCache& operator=(const CatalogCache&) = delete;

public:
    /**
     * Constructs a CatalogCache using separate cache loaders for the database and collection
     * caches.
     *
     * The shutdown flags control whether the database and/or collection cache loaders should be
     * shut down when this CatalogCache is destroyed. This is useful when the loaders are not shared
     * across multiple caches.
     */
    CatalogCache(ServiceContext* service,
                 std::shared_ptr<CatalogCacheLoader> databaseCacheLoader,
                 std::shared_ptr<CatalogCacheLoader> collectionCacheLoader,
                 bool cascadeDatabaseCacheLoaderShutdown = true,
                 bool cascadeCollectionCacheLoaderShutdown = true,
                 StringData kind = ""_sd);

    /**
     * Constructs a CatalogCache using a single cache loader for both database and collection
     * caches.
     */
    CatalogCache(ServiceContext* service,
                 std::shared_ptr<CatalogCacheLoader> cacheLoader,
                 StringData kind = ""_sd);

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
     * Blocking method to get collection placement information.
     *
     * If the collection is sharded, returns placement info initialized with a ChunkManager.
     * If the collection is not sharded, returns placement info initialized with the primary shard
     * for the specified database. If an error occurs while loading the metadata, returns a failed
     * status.
     *
     * If the given atClusterTime is so far in the past that it is not possible to construct
     * placement info, returns a StaleClusterTime error.
     */
    StatusWith<CollectionRoutingInfo> getCollectionRoutingInfoAt(OperationContext* opCtx,
                                                                 const NamespaceString& nss,
                                                                 Timestamp atClusterTime,
                                                                 bool allowLocks = false);

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
     * Blocking method to retrieve refreshed collection placement information (ChunkManager).
     */
    virtual StatusWith<CurrentChunkManager> getCollectionPlacementInfoWithRefresh(
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
     * Advances the version in the cache for the given namespace.
     *
     * To be called with the wantedVersion. In the case the passed version is boost::none, uses a
     * which will artificially be greater than any previously created version to force the catalog
     * cache refresh on next causal consistence access.
     */
    void onStaleCollectionVersion(const NamespaceString& nss,
                                  const boost::optional<ShardVersion>& wantedVersion);

    /**
     * Notifies the cache that there is a (possibly) newer collection version on the backing store.
     */
    virtual void advanceCollectionTimeInStore(const NamespaceString& nss,
                                              const ChunkVersion& newVersionInStore);

    /**
     * Notifies the cache that there is a (possibly) newer version on the backing store for all the
     * entries that reference the passed shard. This will trigger an incremental refresh on the next
     * cache access.
     */
    void advanceTimeInStoreForEntriesThatReferenceShard(const ShardId& shardId);

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
                      std::shared_ptr<CatalogCacheLoader> catalogCacheLoader);

        void shutDown() {
            _catalogCacheLoader->shutDown();
        }

    private:
        LookupResult _lookupDatabase(OperationContext* opCtx,
                                     const DatabaseName& dbName,
                                     const ValueHandle& dbType,
                                     const ComparableDatabaseVersion& previousDbVersion);

        std::shared_ptr<CatalogCacheLoader> _catalogCacheLoader;
        ObservableMutex<stdx::mutex> _mutex;
    };

    class CollectionCache : public RoutingTableHistoryCache {
    public:
        CollectionCache(ServiceContext* service,
                        ThreadPoolInterface& threadPool,
                        std::shared_ptr<CatalogCacheLoader> catalogCacheLoader);

        void reportStats(BSONObjBuilder* builder) const;

        void shutDown() {
            _catalogCacheLoader->shutDown();
        }

    private:
        LookupResult _lookupCollection(OperationContext* opCtx,
                                       const NamespaceString& nss,
                                       const ValueHandle& collectionHistory,
                                       const ComparableChunkVersion& previousChunkVersion);

        std::shared_ptr<CatalogCacheLoader> _catalogCacheLoader;
        ObservableMutex<stdx::mutex> _mutex;

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

    // Callers of this internal function that are passing allowLocks must handle allowLocks failures
    // by checking for ErrorCodes::ShardCannotRefreshDueToLocksHeld and addint the full namespace to
    // the exception.
    StatusWith<CachedDatabaseInfo> _getDatabase(OperationContext* opCtx,
                                                const DatabaseName& dbName,
                                                bool allowLocks = false);

    StatusWith<CachedDatabaseInfo> _getDatabaseForCollectionRoutingInfo(OperationContext* opCtx,
                                                                        const NamespaceString& nss,
                                                                        bool allowLocks);

    StatusWith<CollectionRoutingInfo> _getCollectionRoutingInfoAt(
        OperationContext* opCtx,
        const NamespaceString& nss,
        boost::optional<Timestamp> optAtClusterTime,
        bool allowLocks = false);

    StatusWith<RoutingTableHistoryValueHandle> _getCollectionRoutingTable(
        OperationContext* opCtx, const NamespaceString& nss, bool allowLocks = false);

    void _triggerPlacementVersionRefresh(const NamespaceString& nss);

    // (Optional) the kind of catalog cache instantiated. Used for logging and reporting purposes.
    std::string _kind;

    // Executor on which the caches below will execute their blocking work
    ThreadPool _executor;

    // Flags set at construction time to determine whether the database and collection catalog cache
    // loaders should be shut down at destruction time. This allows for independent control if they
    // do not share the same loader instance.
    const bool _cascadeDatabaseCacheLoaderShutdown;
    const bool _cascadeCollectionCacheLoaderShutdown;

    DatabaseCache _databaseCache;

    CollectionCache _collectionCache;

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

/**
 * RAII type that instructs the CatalogCache to set the 'shouldIgnoreUuidMismatch' flag to
 * CollectionRoutingInfo objects returned withing the scope, indicating that routing done used that
 * object will instruct the receiving shards to ignore collection UUID mismatches between the
 * sharding catalog and the local catalog.
 *
 * This is only meant to be used for inconsistency-recovery situations.
 */
class MONGO_MOD_NEEDS_REPLACEMENT RouterRelaxCollectionUUIDConsistencyCheckBlock {
public:
    RouterRelaxCollectionUUIDConsistencyCheckBlock(OperationContext* opCtx);
    RouterRelaxCollectionUUIDConsistencyCheckBlock(
        const RouterRelaxCollectionUUIDConsistencyCheckBlock&) = delete;
    ~RouterRelaxCollectionUUIDConsistencyCheckBlock();

private:
    OperationContext* const _opCtx;
};

}  // namespace mongo
