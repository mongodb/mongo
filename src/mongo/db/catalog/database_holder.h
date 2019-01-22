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

namespace mongo {

class CollectionCatalogEntry;
class Database;
class DatabaseCatalogEntry;
class OperationContext;
class RecordStore;

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

    virtual ~DatabaseHolder() = default;

    DatabaseHolder() = default;

    /**
     * Retrieves an already opened database or returns nullptr. Must be called with the database
     * locked in at least IS-mode.
     */
    virtual Database* getDb(OperationContext* const opCtx, const StringData ns) const = 0;

    /**
     * Retrieves a database reference if it is already opened, or opens it if it hasn't been
     * opened/created yet. Must be called with the database locked in X-mode.
     *
     * @param justCreated Returns whether the database was newly created (true) or it already
     *          existed (false). Can be NULL if this information is not necessary.
     */
    virtual Database* openDb(OperationContext* const opCtx,
                             const StringData ns,
                             bool* const justCreated = nullptr) = 0;

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
    virtual void close(OperationContext* opCtx, const StringData ns) = 0;

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
    virtual std::set<std::string> getNamesWithConflictingCasing(const StringData name) = 0;

    /**
     * Returns a new Collection.
     * This function supports rebuilding indexes during the repair process and should not be used
     * for any other purpose.
     */
    virtual std::unique_ptr<Collection> makeCollection(OperationContext* const opCtx,
                                                       const StringData fullNS,
                                                       OptionalCollectionUUID uuid,
                                                       CollectionCatalogEntry* const details,
                                                       RecordStore* const recordStore,
                                                       DatabaseCatalogEntry* const dbce) = 0;
};

}  // namespace mongo
