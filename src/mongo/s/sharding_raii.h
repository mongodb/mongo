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
     * Ensures that the specified database and collection both exist in the cache and if so, returns
     * it. Otherwise, if it does not exist or any other error occurs, passes that error back.
     */
    static StatusWith<ScopedChunkManager> getExisting(OperationContext* txn,
                                                      const NamespaceString& nss);

    /**
     * Returns the underlying database for which we hold reference.
     */
    DBConfig* db() const {
        return _db.db();
    }

    /**
     * Returns the underlying chunk manager for which we hold reference.
     */
    ChunkManager* cm() const {
        return _cm.get();
    }

private:
    ScopedChunkManager(ScopedShardDatabase db, std::shared_ptr<ChunkManager> cm);

    // Scoped reference to the owning database.
    ScopedShardDatabase _db;

    // Reference to the corresponding chunk manager. Never null.
    std::shared_ptr<ChunkManager> _cm;
};

}  // namespace mongo
