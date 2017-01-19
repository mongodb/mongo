/**
 *    Copyright (C) 2008-2015 MongoDB Inc.
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

#include <set>
#include <string>

#include "mongo/db/repl/optime.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/s/client/shard.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

class ChunkManager;
struct CollectionInfo;
class DatabaseType;
class OperationContext;

/**
 * Represents the cache entry for a database.
 */
class DBConfig {
public:
    DBConfig(const DatabaseType& dbt, repl::OpTime configOpTime);
    ~DBConfig();

    /**
     * The name of the database which this entry caches.
     */
    const std::string& name() const {
        return _name;
    }

    ShardId getPrimaryId();

    /**
     * Returns whether 'enableSharding' has been called for this database.
     */
    bool isShardingEnabled() const {
        return _shardingEnabled;
    }

    /**
     * Removes the specified namespace from the set of collections under this database entry so that
     * from then onwards it will be treated as unsharded.
     *
     * Note that this method doesn't do any writes to the config metadata, but simply drops the
     * specified namespace from the cache.
     */
    void markNSNotSharded(const std::string& ns);

    /**
     * @return whether or not the 'ns' collection is partitioned
     */
    bool isSharded(const std::string& ns);

    // Atomically returns *either* the chunk manager *or* the primary shard for the collection,
    // neither if the collection doesn't exist.
    void getChunkManagerOrPrimary(OperationContext* txn,
                                  const std::string& ns,
                                  std::shared_ptr<ChunkManager>& manager,
                                  std::shared_ptr<Shard>& primary);

    std::shared_ptr<ChunkManager> getChunkManager(OperationContext* txn,
                                                  const std::string& ns,
                                                  bool reload = false,
                                                  bool forceReload = false);
    std::shared_ptr<ChunkManager> getChunkManagerIfExists(OperationContext* txn,
                                                          const std::string& ns,
                                                          bool reload = false,
                                                          bool forceReload = false);

    /**
     * Returns true if it is successful at loading the DBConfig, false if the database is not found,
     * and throws on all other errors.
     */
    bool load(OperationContext* txn);
    bool reload(OperationContext* txn);

    void getAllShardIds(std::set<ShardId>* shardIds);

protected:
    typedef std::map<std::string, CollectionInfo> CollectionInfoMap;
    typedef AtomicUInt64::WordType Counter;

    /**
     * Returns true if it is successful at loading the DBConfig, false if the database is not found,
     * and throws on all other errors.
     * Also returns true without reloading if reloadIteration is not equal to the _reloadCount.
     * This is to avoid multiple threads attempting to reload do duplicate work.
     */
    bool _loadIfNeeded(OperationContext* txn, Counter reloadIteration);

    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (L) Must hold _lock for access.
    // (S) Self synchronizing, no explicit locking needed.
    //
    // Mutex lock order:
    // _hitConfigServerLock -> _lock
    //

    // Name of the database which this entry caches
    const std::string _name;

    // Whether sharding is enabled for this database
    const bool _shardingEnabled;

    // Primary shard id
    ShardId _primaryId;  // (L)

    // Set of collections and lock to protect access
    stdx::mutex _lock;
    CollectionInfoMap _collections;  // (L)

    // OpTime of config server when the database definition was loaded.
    repl::OpTime _configOpTime;  // (L)

    // Ensures that only one thread at a time loads collection configuration data from
    // the config server
    stdx::mutex _hitConfigServerLock;

    // Increments every time this performs a full reload. Since a full reload can take a very
    // long time for very large clusters, this can be used to minimize duplicate work when multiple
    // threads tries to perform full rerload at roughly the same time.
    AtomicUInt64 _reloadCount;  // (S)
};

}  // namespace mongo
