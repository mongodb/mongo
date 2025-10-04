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
#include "mongo/db/database_name.h"
#include "mongo/db/local_catalog/database.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"

#include <memory>
#include <set>
#include <string>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Registry of opened databases.
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
     * Retrieves an already opened database or returns nullptr.
     *
     * The caller must hold the database lock in MODE_IS.
     */
    virtual Database* getDb(OperationContext* opCtx, const DatabaseName& dbName) const = 0;

    /**
     * Checks if a database exists without holding a database-level lock. This class' internal mutex
     * provides concurrency protection around looking up the db name of 'dbName'.
     */
    virtual bool dbExists(OperationContext* opCtx, const DatabaseName& dbName) const = 0;

    /**
     * Retrieves a database reference if it is already opened, or opens it if it hasn't been
     * opened/created yet.
     *
     * The caller must hold the database lock in MODE_IX.
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
     * The caller must hold the database lock in MODE_X and ensure no index builds are in progress
     * on the database.
     */
    virtual void dropDb(OperationContext* opCtx, Database* db) = 0;

    /**
     * Closes the specified database.
     *
     * The caller must hold the database lock in MODE_X. No background jobs must be in progress on
     * the database when this function is called.
     */
    virtual void close(OperationContext* opCtx, const DatabaseName& dbName) = 0;

    /**
     * Closes all opened databases.
     *
     * The caller must hold the global lock in MODE_X and ensure no index builds are in progress on
     * the databases. Will uassert if any background jobs are running when this function is called.
     */
    virtual void closeAll(OperationContext* opCtx) = 0;

    /**
     * Returns the name of the database with conflicting casing if one exists.
     */
    virtual boost::optional<DatabaseName> getNameWithConflictingCasing(
        const DatabaseName& dbName) = 0;

    /**
     * Returns all the database names (including those which are empty).
     *
     * Unlike CollectionCatalog::getAllDbNames(), this returns databases that are empty.
     */
    virtual std::vector<DatabaseName> getNames() = 0;
};

}  // namespace mongo
