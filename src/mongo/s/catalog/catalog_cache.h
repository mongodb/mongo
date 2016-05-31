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

#include <map>
#include <memory>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

class ShardingCatalogClient;
class DBConfig;
class OperationContext;
template <typename T>
class StatusWith;


/**
 * This is the root of the "read-only" hierarchy of cached catalog metadata. It is read only
 * in the sense that it only reads from the persistent store, but never writes to it. Instead
 * writes happen through the ShardingCatalogManager and the cache hierarchy needs to be invalidated.
 */
class CatalogCache {
    MONGO_DISALLOW_COPYING(CatalogCache);

public:
    CatalogCache();

    /**
     * Retrieves the cached metadata for the specified database. The returned value is still
     * owned by the cache and it should not be cached elsewhere, but instead only used as a
     * local variable. The reason for this is so that if the cache gets invalidated, the caller
     * does not miss getting the most up-to-date value.
     *
     * @param dbname The name of the database (must not contain dots, etc).
     * @return The database if it exists, NULL otherwise.
     */
    StatusWith<std::shared_ptr<DBConfig>> getDatabase(OperationContext* txn,
                                                      const std::string& dbName);

    /**
     * Removes the database information for the specified name from the cache, so that the
     * next time getDatabase is called, it will be reloaded.
     */
    void invalidate(const std::string& dbName);

    /**
     * Purges all cached database information, which will cause the data to be reloaded again.
     */
    void invalidateAll();

private:
    typedef std::map<std::string, std::shared_ptr<DBConfig>> ShardedDatabasesMap;

    // Databases catalog map and mutex to protect it
    stdx::mutex _mutex;
    ShardedDatabasesMap _databases;
};

}  // namespace mongo
