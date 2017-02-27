/**
 *    Copyright (C) 2015 MongoDB Inc.
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
#include "mongo/s/chunk_manager.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/string_map.h"

namespace mongo {

class CachedDatabaseInfo;
class CachedCollectionRoutingInfo;
class OperationContext;

/**
 * This is the root of the "read-only" hierarchy of cached catalog metadata. It is read only
 * in the sense that it only reads from the persistent store, but never writes to it. Instead
 * writes happen through the ShardingCatalogManager and the cache hierarchy needs to be invalidated.
 */
class CatalogCache {
    MONGO_DISALLOW_COPYING(CatalogCache);

public:
    CatalogCache();
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
    StatusWith<CachedCollectionRoutingInfo> getCollectionRoutingInfo(OperationContext* opCtx,
                                                                     StringData ns);

    /**
     * Same as getCollectionRoutingInfo above, but in addition causes the namespace to be refreshed
     * and returns a NamespaceNotSharded error if the collection is not sharded.
     */
    StatusWith<CachedCollectionRoutingInfo> getShardedCollectionRoutingInfoWithRefresh(
        OperationContext* opCtx, const NamespaceString& nss);
    StatusWith<CachedCollectionRoutingInfo> getShardedCollectionRoutingInfoWithRefresh(
        OperationContext* opCtx, StringData ns);

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
    void invalidateShardedCollection(StringData ns);

    /**
     * Blocking method, which removes the entire specified database (including its collections) from
     * the cache.
     */
    void purgeDatabase(StringData dbName);

    /**
     * Blocking method, which removes all databases (including their collections) from the cache.
     */
    void purgeAllDatabases();

    /**
     * Blocking method, which refreshes the routing information for the specified collection. If
     * 'existingRoutingInfo' has been specified uses this as a basis to perform an 'incremental'
     * refresh, which only fetches the chunks which changed. Otherwise does a full refresh, fetching
     * all the chunks for the collection.
     *
     * Returns the refreshed routing information if the collection is still sharded or nullptr if it
     * is not. If refresh fails for any reason, throws a DBException.
     *
     * With the exception of ConflictingOperationInProgress, error codes thrown from this method are
     * final in that there is nothing that can be done to remedy them other than pass the error to
     * the user.
     *
     * ConflictingOperationInProgress indicates that the chunk metadata was found to be
     * inconsistent. Since this may be transient, due to the collection being dropped or recreated,
     * the caller must retry the reload up to some configurable number of attempts.
     *
     * NOTE: Should never be called directly and is exposed as public for testing purposes only.
     */
    static std::shared_ptr<ChunkManager> refreshCollectionRoutingInfo(
        OperationContext* opCtx,
        const NamespaceString& nss,
        std::shared_ptr<ChunkManager> existingRoutingInfo);

private:
    // Make the cache entries friends so they can access the private classes below
    friend class CachedDatabaseInfo;
    friend class CachedCollectionRoutingInfo;

    /**
     * Cache entry describing a collection.
     */
    struct CollectionRoutingInfoEntry {
        std::shared_ptr<ChunkManager> routingInfo;

        bool needsRefresh{true};
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
    std::shared_ptr<DatabaseInfoEntry> _getDatabase_inlock(OperationContext* opCtx,
                                                           StringData dbName);

    // Mutex to serialize access to the structures below
    stdx::mutex _mutex;

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
