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
#include "mongo/db/s/sharding_migration_critical_section.h"
#include "mongo/db/s/sharding_state_lock.h"
#include "mongo/s/catalog/type_database_gen.h"

namespace mongo {

class MovePrimarySourceManager;
class OperationContext;

/**
 * Synchronizes access to this shard server's cached database version for Database.
 */
class DatabaseShardingState {
    DatabaseShardingState(const DatabaseShardingState&) = delete;
    DatabaseShardingState& operator=(const DatabaseShardingState&) = delete;

public:
    /**
     * A ShardingStateLock is used on DatabaseShardingState operations in order to ensure
     * synchronization across operations.
     */
    using DSSLock = ShardingStateLock<DatabaseShardingState>;

    DatabaseShardingState(StringData dbName);
    ~DatabaseShardingState() = default;

    /**
     * Obtains the sharding state for the specified database. If it does not exist, it will be
     * created and will remain in memory until the database is dropped.
     *
     * Must be called with some lock held on the database being looked up and the returned
     * pointer must not be stored.
     */
    static DatabaseShardingState* get(OperationContext* opCtx, StringData dbName);

    /**
     * Obtain a pointer to the DatabaseShardingState that remains safe to access without holding
     * a database lock. Should be called instead of the regular get() if no database lock is held.
     * The returned DatabaseShardingState instance should not be modified!
     */
    static std::shared_ptr<DatabaseShardingState> getSharedForLockFreeReads(OperationContext* opCtx,
                                                                            StringData dbName);

    /**
     * Methods to control the databases's critical section. Must be called with the database X lock
     * held.
     */
    void enterCriticalSectionCatchUpPhase(OperationContext* opCtx, DSSLock&, const BSONObj& reason);
    void enterCriticalSectionCommitPhase(OperationContext* opCtx, DSSLock&, const BSONObj& reason);
    void exitCriticalSection(OperationContext* opCtx, const BSONObj& reason);

    auto getCriticalSectionSignal(ShardingMigrationCriticalSection::Operation op, DSSLock&) const {
        return _critSec.getSignal(op);
    }

    auto getCriticalSectionReason(DSSLock&) const {
        return _critSec.getReason() ? _critSec.getReason()->toString() : "Unknown";
    }

    /**
     * Returns the active movePrimary source manager, if one is available.
     */
    MovePrimarySourceManager* getMovePrimarySourceManager(DSSLock&);

    /**
     * Attaches a movePrimary source manager to this database's sharding state. Must be called with
     * the database lock in X mode. May not be called if there is a movePrimary source manager
     * already installed. Must be followed by a call to clearMovePrimarySourceManager.
     */
    void setMovePrimarySourceManager(OperationContext* opCtx,
                                     MovePrimarySourceManager* sourceMgr,
                                     DSSLock&);

    /**
     * Removes a movePrimary source manager from this database's sharding state. Must be called with
     * with the database lock in X mode. May not be called if there isn't a movePrimary source
     * manager installed already through a previous call to setMovePrimarySourceManager.
     */
    void clearMovePrimarySourceManager(OperationContext* opCtx);

private:
    friend DSSLock;

    // Object-wide ResourceMutex to protect changes to the DatabaseShardingState or objects held
    // within.
    Lock::ResourceMutex _stateChangeMutex{"DatabaseShardingState"};

    const std::string _dbName;

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
};

}  // namespace mongo
