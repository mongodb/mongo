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

#include <set>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/database_name.h"
#include "mongo/s/catalog/type_database_gen.h"
#include "mongo/s/database_version.h"
#include "mongo/s/shard_id.h"

namespace mongo {

class OperationContext;

/**
 * Registry of opened databases.
 *
 * This also provides functions to read and write cached information for opened databases, which
 * basically includes version and ID of the primary shard for each database. The concurrency model
 * of this API is implemented as follows:
 *   1. The `Database` class caches the information for the specific database.
 *   2. Getter and setter functions exposed by this class return and write, respectively, a copy of
 *      the cached information for the specific database.
 *   3. Getter and setter functions are synchronized with each other using the same mutex used to
 *      synchronize the database map. This prevents one thread from accessing information from a
 *      database while another is deleting it, for example.
 */
class DatabaseHolder {
public:
    // Operation Context binding.
    static DatabaseHolder* get(ServiceContext* service);
    static DatabaseHolder* get(ServiceContext& service);
    static DatabaseHolder* get(OperationContext* opCtx);
    static void set(ServiceContext* service, std::unique_ptr<DatabaseHolder> databaseHolder);

    DatabaseHolder() = default;
    virtual ~DatabaseHolder() = default;

    /**
     * Retrieves an already opened database or returns nullptr. Must be called with the database
     * locked in at least IS-mode.
     */
    virtual Database* getDb(OperationContext* opCtx, const DatabaseName& dbName) const = 0;

    /**
     * Checks if a database exists without holding a database-level lock. This class' internal mutex
     * provides concurrency protection around looking up the db name of 'dbName'.
     */
    virtual bool dbExists(OperationContext* opCtx, const DatabaseName& dbName) const = 0;

    /**
     * Retrieves a database reference if it is already opened, or opens it if it hasn't been
     * opened/created yet. Must be called with the database locked in X-mode.
     *
     * @param justCreated Returns whether the database was newly created (true) or it already
     *          existed (false). Can be NULL if this information is not necessary.
     */
    virtual Database* openDb(OperationContext* opCtx,
                             const DatabaseName& dbName,
                             bool* justCreated = nullptr) = 0;

    /**
     * Physically drops the specified opened database and removes it from the server's metadata. It
     * doesn't notify the replication subsystem or do any other consistency checks, so it should
     * not be used directly from user commands.
     *
     * Must be called with the specified database locked in X mode. The caller must ensure no index
     * builds are in progress on the database.
     */
    virtual void dropDb(OperationContext* opCtx, Database* db) = 0;

    /**
     * Closes the specified database. Must be called with the database locked in X-mode.
     * No background jobs must be in progress on the database when this function is called.
     */
    virtual void close(OperationContext* opCtx, const DatabaseName& dbName) = 0;

    /**
     * Closes all opened databases. Must be called with the global lock acquired in X-mode.
     * Will uassert if any background jobs are running when this function is called.
     *
     * The caller must hold the global X lock and ensure there are no index builds in progress.
     */
    virtual void closeAll(OperationContext* opCtx) = 0;

    /**
     * Returns the set of existing database names that differ only in casing.
     */
    virtual std::set<DatabaseName> getNamesWithConflictingCasing(const DatabaseName& dbName) = 0;

    /**
     * Returns all the database names (including those which are empty).
     *
     * Unlike CollectionCatalog::getAllDbNames(), this returns databases that are empty.
     */
    virtual std::vector<DatabaseName> getNames() = 0;

    /**
     * Caches the information of the database with the specific name if the database is open,
     * otherwise it does nothing.
     *
     * The database must be locked in X mode when calling this function.
     */
    virtual void setDbInfo(OperationContext* opCtx,
                           const DatabaseName& dbName,
                           const DatabaseType& dbInfo) = 0;

    /**
     * Clears the cached information of the database with the specific name if the database is open,
     * otherwise it does nothing.
     *
     * The database must be locked in IX mode when calling this function.
     */
    virtual void clearDbInfo(OperationContext* opCtx, const DatabaseName& dbName) = 0;

    /**
     * Returns the version of the database with the specific name if the database is open and the
     * version is known, otherwise it returns `boost::none`.
     */
    virtual boost::optional<DatabaseVersion> getDbVersion(OperationContext* opCtx,
                                                          const DatabaseName& dbName) const = 0;

    /**
     * Returns the primary shard ID of the database with the specific name if the database is open
     * and the primary shard ID is known, otherwise it returns `boost::none`.
     *
     * The database must be locked in IS mode when calling this function.
     */
    virtual boost::optional<ShardId> getDbPrimary(OperationContext* opCtx,
                                                  const DatabaseName& dbName) const = 0;
};

}  // namespace mongo
