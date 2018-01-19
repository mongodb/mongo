/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/disallow_copying.h"
#include "mongo/base/string_data.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/s/catalog_cache_loader.h"
#include "mongo/s/chunk_manager.h"
#include "mongo/s/client/shard.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/string_map.h"

namespace mongo {

class BSONObjBuilder;
class CachedDatabaseInfo;
class CachedCollectionRoutingInfo;
class OperationContext;

static constexpr int kMaxNumStaleVersionRetries = 10;

/**
 * This is the root of the "read-only" hierarchy of cached catalog metadata. It is read only
 * in the sense that it only reads from the persistent store, but never writes to it. Instead
 * writes happen through the ShardingCatalogManager and the cache hierarchy needs to be invalidated.
 */
class CatalogCache {
    MONGO_DISALLOW_COPYING(CatalogCache);

public:
    CatalogCache(CatalogCacheLoader& cacheLoader);
    ~CatalogCache();

    /**
     * Retrieves the cached metadata for the specified database. The returned value is still owned
     * by the cache and should not be kept elsewhere. I.e., it should only be used as a local
     * variable. The reason for this is so that if the cache gets invalidated, the caller does not
     * miss getting the most up-to-date value.
     *
     * Returns the database cache entry if the database exists or a failed status otherwise.
     */
    StatusWith<CachedDatabaseInfo> getDatabase(OperationContext* opCtx, StringData dbName);

    /**
     * Blocking shortcut method to get a specific sharded collection from a given database using the
     * complete namespace. If the collection is sharded returns a ScopedChunkManager initialized
     * with ChunkManager. If the collection is not sharded, returns a ScopedChunkManager initialized
     * with the primary shard for the specified database. If an error occurs loading the metadata
     * returns a failed status.
     */
    StatusWith<CachedCollectionRoutingInfo> getCollectionRoutingInfo(OperationContext* opCtx,
                                                                     const NamespaceString& nss);

    /**
     * Same as getCollectionRoutingInfo above, but in addition causes the namespace to be refreshed.
     */
    StatusWith<CachedCollectionRoutingInfo> getCollectionRoutingInfoWithRefresh(
        OperationContext* opCtx, const NamespaceString& nss);

    /**
     * Same as getCollectionRoutingInfoWithRefresh above, but in addition returns a
     * NamespaceNotSharded error if the collection is not sharded.
     */
    StatusWith<CachedCollectionRoutingInfo> getShardedCollectionRoutingInfoWithRefresh(
        OperationContext* opCtx, const NamespaceString& nss);

    /**
     * Non-blocking method to be called whenever using the specified routing table has encountered a
     * stale config exception. Returns immediately and causes the routing table to be refreshed the
     * next time getCollectionRoutingInfo is called. Does nothing if the routing table has been
     * refreshed already.
     */
    void onStaleConfigError(CachedCollectionRoutingInfo&&);

    /**
     * Non-blocking method, which indiscriminately causes the routing table for the specified
     * namespace to be refreshed the next time getCollectionRoutingInfo is called.
     */
    void invalidateShardedCollection(const NamespaceString& nss);

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

private:
    // Make the cache entries friends so they can access the private classes below
    friend class CachedDatabaseInfo;
    friend class CachedCollectionRoutingInfo;

    /**
     * Cache entry describing a collection.
     */
    struct CollectionRoutingInfoEntry {
        // Specifies whether this cache entry needs a refresh (in which case routingInfo should not
        // be relied on) or it doesn't, in which case there should be a non-null routingInfo.
        bool needsRefresh{true};

        // Contains a notification to be waited on for the refresh to complete (only available if
        // needsRefresh is true)
        std::shared_ptr<Notification<Status>> refreshCompletionNotification;

        // Contains the cached routing information (only available if needsRefresh is false)
        std::shared_ptr<ChunkManager> routingInfo;
    };

    /**
     * Cache entry describing a database.
     */
    struct DatabaseInfoEntry {
        ShardId primaryShardId;

        bool shardingEnabled;

        StringMap<CollectionRoutingInfoEntry> collections;
    };

    using DatabaseInfoMap = StringMap<std::shared_ptr<DatabaseInfoEntry>>;

    /**
     * Ensures that the specified database is in the cache, loading it if necessary. If the database
     * was not in cache, all the sharded collections will be in the 'needsRefresh' state.
     */
    std::shared_ptr<DatabaseInfoEntry> _getDatabase(OperationContext* opCtx, StringData dbName);

    /**
     * Non-blocking call which schedules an asynchronous refresh for the specified namespace. The
     * namespace must be in the 'needRefresh' state.
     */
    void _scheduleCollectionRefresh(WithLock,
                                    std::shared_ptr<DatabaseInfoEntry> dbEntry,
                                    std::shared_ptr<ChunkManager> existingRoutingInfo,
                                    NamespaceString const& nss,
                                    int refreshAttempt);

    // Interface from which chunks will be retrieved
    CatalogCacheLoader& _cacheLoader;

    // Encapsulates runtime statistics across all collections in the catalog cache
    struct Stats {
        // Counts how many times threads hit stale config exception (which is what triggers metadata
        // refreshes)
        AtomicInt64 countStaleConfigErrors{0};

        // Cumulative, always-increasing counter of how much time threads waiting for refresh
        // combined
        AtomicInt64 totalRefreshWaitTimeMicros{0};

        // Tracks how many incremental refreshes are waiting to complete currently
        AtomicInt64 numActiveIncrementalRefreshes{0};

        // Cumulative, always-increasing counter of how many incremental refreshes have been kicked
        // off
        AtomicInt64 countIncrementalRefreshesStarted{0};

        // Tracks how many full refreshes are waiting to complete currently
        AtomicInt64 numActiveFullRefreshes{0};

        // Cumulative, always-increasing counter of how many full refreshes have been kicked off
        AtomicInt64 countFullRefreshesStarted{0};

        // Cumulative, always-increasing counter of how many full or incremental refreshes failed
        // for whatever reason
        AtomicInt64 countFailedRefreshes{0};

        /**
         * Reports the accumulated statistics for serverStatus.
         */
        void report(BSONObjBuilder* builder) const;

    } _stats;

    // Mutex to serialize access to the structures below
    mutable stdx::mutex _mutex;

    // Map from DB name to the info for that database
    DatabaseInfoMap _databases;
};

/**
 * Constructed exclusively by the CatalogCache, contains a reference to the cached information for
 * the specified database.
 */
class CachedDatabaseInfo {
public:
    const ShardId& primaryId() const;

    bool shardingEnabled() const;

private:
    friend class CatalogCache;

    CachedDatabaseInfo(std::shared_ptr<CatalogCache::DatabaseInfoEntry> db);

    std::shared_ptr<CatalogCache::DatabaseInfoEntry> _db;
};

/**
 * Constructed exclusively by the CatalogCache contains a reference to the routing information for
 * the specified collection.
 */
class CachedCollectionRoutingInfo {
public:
    /**
     * Returns the ID of the primary shard for the database owining this collection, regardless of
     * whether it is sharded or not.
     */
    const ShardId& primaryId() const {
        return _primaryId;
    }

    /**
     * If the collection is sharded, returns a chunk manager for it. Otherwise, nullptr.
     */
    std::shared_ptr<ChunkManager> cm() const {
        return _cm;
    }

    /**
     * If the collection is not sharded, returns its primary shard. Otherwise, nullptr.
     */
    std::shared_ptr<Shard> primary() const {
        return _primary;
    }

private:
    friend class CatalogCache;

    CachedCollectionRoutingInfo(ShardId primaryId, std::shared_ptr<ChunkManager> cm);

    CachedCollectionRoutingInfo(ShardId primaryId,
                                NamespaceString nss,
                                std::shared_ptr<Shard> primary);

    // The id of the primary shard containing the database
    ShardId _primaryId;

    // Reference to the corresponding chunk manager (if sharded) or null
    std::shared_ptr<ChunkManager> _cm;

    // Reference to the primary of the database (if not sharded) or null
    NamespaceString _nss;
    std::shared_ptr<Shard> _primary;
};

}  // namespace mongo
