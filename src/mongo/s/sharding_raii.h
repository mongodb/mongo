/**
 *    Copyright (C) 2016 MongoDB Inc.
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
#include "mongo/s/chunk_manager.h"
#include "mongo/s/config.h"

namespace mongo {

class DBConfig;
class OperationContext;
template <typename T>
class StatusWith;

class ScopedShardDatabase {
    MONGO_DISALLOW_COPYING(ScopedShardDatabase);

public:
    ScopedShardDatabase(ScopedShardDatabase&&) = default;
    ~ScopedShardDatabase();

    /**
     * Ensures that the specified database exists in the cache and if it does, returns it.
     * Otherwise, either returns NamespaceNotFound if the database does not exist, or any other
     * error code indicating why the database could not be loaded.
     */
    static StatusWith<ScopedShardDatabase> getExisting(OperationContext* txn, StringData dbName);

    /**
     * If the specified database exists already, loads it in the cache (if not already there) and
     * returns it. Otherwise, if it does not exis, this call will implicitly create it as
     * non-sharded.
     */
    static StatusWith<ScopedShardDatabase> getOrCreate(OperationContext* txn, StringData dbName);

    /**
     * Returns the underlying database cache entry.
     */
    DBConfig* db() const {
        return _db.get();
    }

    /**
     * This method is here only for compatibility with the legacy M/R code, which requires a shared
     * reference to the underlying database. It should not be used in new code.
     */
    std::shared_ptr<DBConfig> getSharedDbReference() const {
        return _db;
    }

private:
    explicit ScopedShardDatabase(std::shared_ptr<DBConfig> db);

    // Reference to the corresponding database. Never null.
    std::shared_ptr<DBConfig> _db;
};

class ScopedChunkManager {
    MONGO_DISALLOW_COPYING(ScopedChunkManager);

public:
    ScopedChunkManager(ScopedChunkManager&&) = default;
    ~ScopedChunkManager();

    /**
     * If the specified namespace is sharded, returns a ScopedChunkManager initialized with that
     * collection's routing information. If it is not, the object returned is initialized with the
     * database primary node on which the unsharded collection must reside.
     *
     * Returns NamespaceNotFound if the database does not exist, or any other error indicating
     * problem communicating with the config server.
     */
    static StatusWith<ScopedChunkManager> get(OperationContext* txn, const NamespaceString& nss);

    /**
     * If the database holding the specified namespace does not exist, creates it and then behaves
     * like the 'get' method above.
     */
    static StatusWith<ScopedChunkManager> getOrCreate(OperationContext* txn,
                                                      const NamespaceString& nss);

    /**
     * If the specified database and collection do not exist in the cache, tries to load them from
     * the config server and returns a reference. If they are already in the cache, makes a call to
     * the config server to check if there are any incremental updates to the collection chunk
     * metadata and if so incorporates those. Otherwise, if it does not exist or any other error
     * occurs, passes that error back.
     */
    static StatusWith<ScopedChunkManager> refreshAndGet(OperationContext* txn,
                                                        const NamespaceString& nss);

    /**
     * Returns the underlying database for which we hold reference.
     */
    DBConfig* db() const {
        return _db.db();
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
    ScopedChunkManager(ScopedShardDatabase db, std::shared_ptr<ChunkManager> cm);

    ScopedChunkManager(ScopedShardDatabase db, std::shared_ptr<Shard> primary);

    // Scoped reference to the owning database.
    ScopedShardDatabase _db;

    // Reference to the corresponding chunk manager (if sharded) or null
    std::shared_ptr<ChunkManager> _cm;

    // Reference to the primary of the database (if not sharded) or null
    std::shared_ptr<Shard> _primary;
};

}  // namespace mongo
