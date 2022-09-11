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

class MovePrimarySourceManager;

enum class DSSAcquisitionMode { kShared, kExclusive };

/**
 * Synchronizes access to this shard server's cached database version for Database.
 */
class DatabaseShardingState {
    DatabaseShardingState(const DatabaseShardingState&) = delete;
    DatabaseShardingState& operator=(const DatabaseShardingState&) = delete;

public:
    DatabaseShardingState(const DatabaseName& dbName);
    ~DatabaseShardingState() = default;

    /**
     * Obtains the sharding state for the specified database along with a resource lock protecting
     * it from modifications, which will be held until the object goes out of scope.
     */
    class ScopedDatabaseShardingState {
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

        ScopedDatabaseShardingState(OperationContext* opCtx,
                                    const DatabaseName& dbName,
                                    LockMode mode);

        Lock::ResourceLock _lock;
        DatabaseShardingState* _dss;
    };
    static ScopedDatabaseShardingState assertDbLockedAndAcquire(OperationContext* opCtx,
                                                                const DatabaseName& dbName,
                                                                DSSAcquisitionMode mode);
    static ScopedDatabaseShardingState acquire(OperationContext* opCtx,
                                               const DatabaseName& dbName,
                                               DSSAcquisitionMode mode);

    /**
     * Returns the name of the database related to the current sharding state.
     */
    const DatabaseName& getDbName() const {
        return _dbName;
    }

    /**
     * Methods to control the databases's critical section. Must be called with the database X lock
     * held.
     */
    void enterCriticalSectionCatchUpPhase(OperationContext* opCtx, const BSONObj& reason);
    void enterCriticalSectionCommitPhase(OperationContext* opCtx, const BSONObj& reason);
    void exitCriticalSection(OperationContext* opCtx, const BSONObj& reason);

    auto getCriticalSectionSignal(ShardingMigrationCriticalSection::Operation op) const {
        return _critSec.getSignal(op);
    }

    auto getCriticalSectionReason() const {
        return _critSec.getReason() ? _critSec.getReason()->toString() : "Unknown";
    }

    /**
     * Returns the active movePrimary source manager, if one is available.
     */
    bool isMovePrimaryInProgress() const {
        return _sourceMgr;
    }

    /**
     * Attaches a movePrimary source manager to this database's sharding state. Must be called with
     * the database lock in X mode. May not be called if there is a movePrimary source manager
     * already installed. Must be followed by a call to clearMovePrimarySourceManager.
     */
    void setMovePrimarySourceManager(OperationContext* opCtx, MovePrimarySourceManager* sourceMgr);

    /**
     * Removes a movePrimary source manager from this database's sharding state. Must be called with
     * with the database lock in X mode. May not be called if there isn't a movePrimary source
     * manager installed already through a previous call to setMovePrimarySourceManager.
     */
    void clearMovePrimarySourceManager(OperationContext* opCtx);

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

    /**
     * Cancel any ongoing database metadata refresh.
     */
    void cancelDbMetadataRefresh();

private:
    struct DbMetadataRefresh {
        DbMetadataRefresh(SharedSemiFuture<void> future, CancellationSource cancellationSource)
            : future(std::move(future)), cancellationSource(std::move(cancellationSource)){};

        // Tracks the ongoing database metadata refresh.
        SharedSemiFuture<void> future;

        // Cancellation source to cancel the ongoing database metadata refresh.
        CancellationSource cancellationSource;
    };

    // Object-wide ResourceMutex to protect changes to the DatabaseShardingState or objects held
    // within.
    Lock::ResourceMutex _stateChangeMutex{"DatabaseShardingState"};

    const DatabaseName _dbName;

    // Modifying the state below requires holding the DBLock in X mode; holding the DBLock in any
    // mode is acceptable for reading it. (Note: accessing this class at all requires holding the
    // DBLock in some mode).

    ShardingMigrationCriticalSection _critSec;

    // If this database is serving as a source shard for a movePrimary, the source manager will be
    // non-null. To write this value, there needs to be X-lock on the database in order to
    // synchronize with other callers which will read the source manager.
    //
    // NOTE: The source manager is not owned by this class.
    MovePrimarySourceManager* _sourceMgr{nullptr};

    // Tracks the ongoing database metadata refresh. Possibly keeps a future for other threads to
    // wait on it, and a cancellation source to cancel the ongoing database metadata refresh.
    boost::optional<DbMetadataRefresh> _dbMetadataRefresh;
};

}  // namespace mongo
