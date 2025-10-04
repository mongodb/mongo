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

#include "mongo/db/database_name.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/versioning_protocol/database_version.h"

#include <shared_mutex>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Each shard node process (primary or secondary) has one instance of this object for each database
 * whose primary is placed on, or is currently being movePrimary'd to, the current shard. It sits on
 * the second level of the hierarchy of the Shard Role runtime-authoritative caches (along with
 * CollectionShardingState) and contains sharding-related information about the database, such as
 * its database version.
 *
 * SYNCHRONIZATION: Some methods might require holding a database level lock, so be sure to check
 * the function-level comments for details.
 */
class DatabaseShardingState {
    DatabaseShardingState(const DatabaseShardingState&) = delete;
    DatabaseShardingState& operator=(const DatabaseShardingState&) = delete;

public:
    DatabaseShardingState() = default;
    virtual ~DatabaseShardingState() = default;

    /**
     * Obtains the sharding state for the specified database along with a lock in shared mode, which
     * will be held until the object goes out of scope.
     */
    class ScopedDatabaseShardingState {
        using LockType = std::variant<std::shared_lock<std::shared_mutex>,   // NOLINT
                                      std::unique_lock<std::shared_mutex>>;  // NOLINT

    public:
        ScopedDatabaseShardingState(ScopedDatabaseShardingState&&);

        ~ScopedDatabaseShardingState();

        DatabaseShardingState* operator->() const {
            return _dss;
        }

        DatabaseShardingState& operator*() const {
            return *_dss;
        }

    private:
        friend class DatabaseShardingState;
        friend class DatabaseShardingRuntime;
        friend class DatabaseShardingStateMock;

        ScopedDatabaseShardingState(LockType lock, DatabaseShardingState* dss);

        static ScopedDatabaseShardingState acquireScopedDatabaseShardingState(
            OperationContext* opCtx, const DatabaseName& dbName, LockMode mode);

        LockType _lock;
        DatabaseShardingState* _dss;
    };

    static ScopedDatabaseShardingState acquire(OperationContext* opCtx, const DatabaseName& dbName);

    static ScopedDatabaseShardingState assertDbLockedAndAcquire(OperationContext* opCtx,
                                                                const DatabaseName& dbName);

    /**
     * Returns the names of the databases that have a DatabaseShardingState.
     */
    static std::vector<DatabaseName> getDatabaseNames(OperationContext* opCtx);

    /**
     * Checks that the cached database version matches the one attached to the operation, which
     * means that the operation is routed to the right shard (database owner).
     *
     * Throws `StaleDbRoutingVersion` exception when the critical section is taken, there is no
     * cached database version, or the cached database version does not match the one sent by the
     * client.
     */
    virtual void checkDbVersionOrThrow(OperationContext* opCtx) const = 0;

    virtual void checkDbVersionOrThrow(OperationContext* opCtx,
                                       const DatabaseVersion& receivedVersion) const = 0;

    /**
     * Checks that the current shard server is the primary for the given database, throwing
     * `IllegalOperation` if not.
     */
    virtual void assertIsPrimaryShardForDb(OperationContext* opCtx) const = 0;

    /**
     * Returns `true` whether a `movePrimary` operation on this database is in progress, `false`
     * otherwise.
     */
    virtual bool isMovePrimaryInProgress() const = 0;
};


/**
 * Singleton factory to instantiate DatabaseShardingState objects specific to the type of instance
 * which is running.
 */
class DatabaseShardingStateFactory {
    DatabaseShardingStateFactory(const DatabaseShardingStateFactory&) = delete;
    DatabaseShardingStateFactory& operator=(const DatabaseShardingStateFactory&) = delete;

public:
    static void set(ServiceContext* service, std::unique_ptr<DatabaseShardingStateFactory> factory);
    static void clear(ServiceContext* service);

    virtual ~DatabaseShardingStateFactory() = default;

    /**
     * Called by the DatabaseShardingState::acquire method once per newly cached database. It is
     * invoked under a mutex and must not acquire any locks or do blocking work.
     *
     * Implementations must be thread-safe when called from multiple threads.
     */
    virtual std::unique_ptr<DatabaseShardingState> make(const DatabaseName& dbName) = 0;

protected:
    DatabaseShardingStateFactory() = default;
};

}  // namespace mongo
