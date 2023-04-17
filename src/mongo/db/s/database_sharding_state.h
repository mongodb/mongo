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

#include "mongo/bson/bsonobj.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/s/sharding_migration_critical_section.h"
#include "mongo/s/catalog/type_database_gen.h"

namespace mongo {

/**
 * Each shard node process (primary or secondary) has one instance of this object for each database
 * whose primary is placed on, or is currently being movePrimary'd to, the current shard. It sits on
 * the second level of the hierarchy of the Shard Role runtime-authoritative caches (along with
 * CollectionShardingState) and contains sharding-related information about the database, such as
 * its database version.
 *
 * SYNCHRONISATION: Requires some combination of the DB lock and the DSS lock, but different methods
 * have different requirements though, so be sure to check the function-level comments for details.
 */
class DatabaseShardingState {
public:
    DatabaseShardingState(const DatabaseName& dbName);
    virtual ~DatabaseShardingState() = default;

    DatabaseShardingState(const DatabaseShardingState&) = delete;
    DatabaseShardingState& operator=(const DatabaseShardingState&) = delete;

    /**
     * Obtains the sharding state for the specified database along with a resource lock in exclusive
     * mode, which will be held until the object goes out of scope.
     */
    class ScopedExclusiveDatabaseShardingState {
    public:
        DatabaseShardingState* operator->() const {
            return _dss;
        }

        DatabaseShardingState& operator*() const {
            return *_dss;
        }

    private:
        friend class DatabaseShardingState;

        ScopedExclusiveDatabaseShardingState(Lock::ResourceLock lock, DatabaseShardingState* dss);

        Lock::ResourceLock _lock;
        DatabaseShardingState* _dss;
    };

    /**
     * Obtains the sharding state for the specified database along with a resource lock in shared
     * mode, which will be held until the object goes out of scope.
     */
    class ScopedSharedDatabaseShardingState : public ScopedExclusiveDatabaseShardingState {
    public:
        const DatabaseShardingState* operator->() const {
            return _dss;
        }

        const DatabaseShardingState& operator*() const {
            return *_dss;
        }

    private:
        friend class DatabaseShardingState;

        ScopedSharedDatabaseShardingState(Lock::ResourceLock lock, DatabaseShardingState* dss);
    };

    static ScopedExclusiveDatabaseShardingState acquireExclusive(OperationContext* opCtx,
                                                                 const DatabaseName& dbName);

    static ScopedSharedDatabaseShardingState acquireShared(OperationContext* opCtx,
                                                           const DatabaseName& dbName);

    static ScopedExclusiveDatabaseShardingState assertDbLockedAndAcquireExclusive(
        OperationContext* opCtx, const DatabaseName& dbName);

    static ScopedSharedDatabaseShardingState assertDbLockedAndAcquireShared(
        OperationContext* opCtx, const DatabaseName& dbName);

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
    static void assertMatchingDbVersion(OperationContext* opCtx, const DatabaseName& dbName);
    static void assertMatchingDbVersion(OperationContext* opCtx,
                                        const DatabaseName& dbName,
                                        const DatabaseVersion& receivedVersion);

    /**
     * Checks that the current shard server is the primary for the given database, throwing
     * `IllegalOperation` if not.
     */
    static void assertIsPrimaryShardForDb(OperationContext* opCtx, const DatabaseName& dbName);

    /**
     * Returns the name of the database related to the current sharding state.
     */
    const DatabaseName& getDbName() const {
        return _dbName;
    }

    /**
     * Sets this node's cached database info.
     *
     * The caller must hold the database lock in MODE_IX.
     */
    void setDbInfo(OperationContext* opCtx, const DatabaseType& dbInfo);

    /**
     * Resets this node's cached database info.
     *
     * NOTE: Only the thread that refreshes the database metadata (which calls the function
     * `onDbVersionMismatch`) actually needs to change the default initialization of
     * `cancelOngoingRefresh`. This parameter must be ignored in any other case.
     *
     * The caller must hold the database lock in MODE_IX.
     */
    void clearDbInfo(OperationContext* opCtx, bool cancelOngoingRefresh = true);

    /**
     * Returns this node's cached  database version if the database info is cached, otherwise
     * it returns `boost::none`.
     */
    boost::optional<DatabaseVersion> getDbVersion(OperationContext* opCtx) const;

    /**
     * Methods to control the databases's critical section. Must be called with the database X lock
     * held.
     */
    void enterCriticalSectionCatchUpPhase(OperationContext* opCtx, const BSONObj& reason);
    void enterCriticalSectionCommitPhase(OperationContext* opCtx, const BSONObj& reason);
    void exitCriticalSection(OperationContext* opCtx, const BSONObj& reason);
    void exitCriticalSectionNoChecks(OperationContext* opCtx);

    auto getCriticalSectionSignal(ShardingMigrationCriticalSection::Operation op) const {
        return _critSec.getSignal(op);
    }

    auto getCriticalSectionReason() const {
        return _critSec.getReason();
    }

    /**
     * Returns `true` whether a `movePrimary` operation on this database is in progress, `false`
     * otherwise.
     */
    bool isMovePrimaryInProgress() const {
        return _movePrimaryInProgress;
    }

    /**
     * Declares that a `movePrimary` operation on this database is in progress. This causes write
     * operations on this database to fail with the `MovePrimaryInProgress` error.
     *
     * Must be called with the database locked in X mode.
     */
    void setMovePrimaryInProgress(OperationContext* opCtx);

    /**
     * Declares that the `movePrimary` operation on this database is over. This re-enables write
     * operations on this database.
     *
     * Must be called with the database locked in IX mode.
     */
    void unsetMovePrimaryInProgress(OperationContext* opCtx);

    /**
     * Sets the database metadata refresh future for other threads to wait on it.
     */
    void setDbMetadataRefreshFuture(SharedSemiFuture<void> future,
                                    CancellationSource cancellationSource);

    /**
     * If there is an ongoing database metadata refresh, returns the future to wait on it, otherwise
     * `boost::none`.
     */
    boost::optional<SharedSemiFuture<void>> getDbMetadataRefreshFuture() const;

    /**
     * Resets the database metadata refresh future to `boost::none`.
     */
    void resetDbMetadataRefreshFuture();

private:
    struct DbMetadataRefresh {
        DbMetadataRefresh(SharedSemiFuture<void> future, CancellationSource cancellationSource)
            : future(std::move(future)), cancellationSource(std::move(cancellationSource)){};

        // Tracks the ongoing database metadata refresh.
        SharedSemiFuture<void> future;

        // Cancellation source to cancel the ongoing database metadata refresh.
        CancellationSource cancellationSource;
    };

    /**
     * Cancel any ongoing database metadata refresh.
     */
    void _cancelDbMetadataRefresh();

    const DatabaseName _dbName;

    // This node's cached database info.
    boost::optional<DatabaseType> _dbInfo;

    // Modifying the state below requires holding the DBLock in X mode; holding the DBLock in any
    // mode is acceptable for reading it. (Note: accessing this class at all requires holding the
    // DBLock in some mode).

    // Tracks the movePrimary critical section state for this collection.
    ShardingMigrationCriticalSection _critSec;

    // Is `true` when this database is serving as a source shard for a movePrimary, `false`
    // otherwise.
    bool _movePrimaryInProgress{false};

    // Tracks the ongoing database metadata refresh. Possibly keeps a future for other threads to
    // wait on it, and a cancellation source to cancel the ongoing database metadata refresh.
    boost::optional<DbMetadataRefresh> _dbMetadataRefresh;

    /**
     * If there is cached database info, returns `true` if the current shard is the primary shard
     * for the database of the current sharding state. If there is no cached database info, returns
     * `boost::none`.
     *
     * This method is unsafe to use since it doesn't honor the critical section.
     */
    boost::optional<bool> _isPrimaryShardForDb(OperationContext* opCtx) const;

    // Permit the `getDatabaseVersion` command to access the private method `_isPrimaryShardForDb`.
    friend class GetDatabaseVersionCmd;
};

}  // namespace mongo
