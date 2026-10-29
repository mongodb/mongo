// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/database.h"
#include "mongo/util/modules.h"

#include <memory>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Registry of opened databases.
 */
class [[MONGO_MOD_PUBLIC]] DatabaseHolder {
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
