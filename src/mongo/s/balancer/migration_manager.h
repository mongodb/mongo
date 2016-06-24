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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include "mongo/executor/task_executor.h"
#include "mongo/s/balancer/balancer_chunk_selection_policy.h"
#include "mongo/s/catalog/dist_lock_manager.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

class OperationContext;
class Status;
class MigrationSecondaryThrottleOptions;

// Uniquely identifies a migration, regardless of shard and version.
typedef std::string MigrationIdentifier;
typedef std::map<MigrationIdentifier, Status> MigrationStatuses;

/**
 * Manages and executes parallel migrations for the balancer.
 *
 * TODO: for v3.6, remove code making compatible with v3.2 shards that take distlock.
 */
class MigrationManager {
    MONGO_DISALLOW_COPYING(MigrationManager);

public:
    MigrationManager() = default;
    ~MigrationManager() = default;

    /**
     * A blocking method that attempts to schedule all the migrations specified in
     * "candidateMigrations". Takes the distributed lock for each collection with a chunk being
     * migrated.
     *
     * Returns a map of migration Status objects to indicate the success/failure of each migration.
     */
    MigrationStatuses scheduleMigrations(
        OperationContext* txn,
        const BalancerChunkSelectionPolicy::MigrateInfoVector& candidateMigrations);

private:
    /**
     * Holds the data associated with an ongoing migration. Stores a callback handle for the
     * moveChunk command when one is scheduled. Also holds a flag that indicates the source shard is
     * v3.2 and must take the distributed lock itself.
     */
    struct Migration {
        Migration(const MigrateInfo& migrateInfo,
                  boost::optional<executor::TaskExecutor::CallbackHandle> callbackHandle);

        void setCallbackHandle(executor::TaskExecutor::CallbackHandle callbackHandle);
        void clearCallbackHandle();

        // Pointer to the chunk details.
        const MigrateInfo* chunkInfo;

        // Callback handle for the active moveChunk request. If no migration is active for the chunk
        // specified in "chunkInfo", this won't be set.
        boost::optional<executor::TaskExecutor::CallbackHandle> moveChunkCallbackHandle;

        // Indicates that the first moveChunk request failed with LockBusy. The second attempt must
        // be made without the balancer holding the collection distlock. This is necessary for
        // compatibility with a v3.2 shard, which expects to take the distlock itself.
        bool oldShard;
    };

    /**
     * Manages and maintains a collection distlock, which should normally be held by the balancer,
     * but in the case of a migration with a v3.2 source shard the balancer must release it in order
     * to allow the shard to acquire it.
     */
    struct DistLockTracker {
        DistLockTracker(boost::optional<DistLockManager::ScopedDistLock> distlock);

        // Holds the distributed lock, if the balancer should hold it for the migration. If this is
        // empty, then a shard has the distlock.
        boost::optional<DistLockManager::ScopedDistLock> distributedLock;

        // The number of migrations that are currently using the balancer held distributed lock.
        int migrationCounter;
    };

    /**
     * Blocking function that schedules all the migrations prepared in "_activeMigrations" and then
     * waits for them all to complete. This is also where the distributed locks are taken. Some
     * migrations may be rescheduled for a recursive call of this function if there are distributed
     * lock conflicts. A lock conflict can occur when:
     *     1) The source shard of a migration is v3.2 and expects to take the lock itself and the
     *        balancer already holds it for a different migration.
     *     2) A v3.2 shard already has the distlock, so it isn't free for either the balancer to
     *        take or another v3.2 shard.
     * All lock conflicts are resolved by waiting for all of the scheduled migrations to complete,
     * at which point all the locks are safely released.
     *
     * All the moveChunk command Status results are placed in "migrationStatuses".
     */
    void _executeMigrations(OperationContext* txn,
                            std::map<MigrationIdentifier, Status>* migrationStatuses);

    /**
     * Callback function that checks a remote command response for errors. If there is a LockBusy
     * error, the first time this happens the shard starting the migration is assumed to be v3.2 and
     * is marked such and rescheduled. On other errors, the migration is abandoned. Places all of
     * the Status results from the moveChunk commands in "migrationStatuses".
     */
    void _checkMigrationCallback(
        const executor::TaskExecutor::RemoteCommandCallbackArgs& callbackArgs,
        OperationContext* txn,
        Migration* migration,
        std::map<MigrationIdentifier, Status>* migrationStatuses);

    /**
     * Goes through the callback handles in "_activeMigrations" and waits for the moveChunk commands
     * to return.
     */
    void _waitForMigrations(OperationContext* txn) const;

    /**
     * Adds "migration" to "_rescheduledMigrations" vector.
     */
    void _rescheduleMigration(const Migration& migration);

    /**
     * Attempts to take a distlock for collection "ns", if appropriate. It may
     *     1) Take the distlock for the balancer and initialize the counter to 1.
     *     2) Increment the counter on the distlock that the balancer already holds.
     *     3) Initialize the counter to 0 to indicate a migration with a v3.2 shard, where the shard
     *        will take the distlock.
     *
     * If none of these actions are possible because of a lock conflict (shard can't take the lock
     * if the balancer already holds it, or vice versa) or if the lock is unavailable, returns
     * false to indicate that the migration cannot proceed right now. If the lock could not be
     * taken because of a lock conflict as described, then the migration is rescheduled; otherwise
     * it is abandoned.
     *
     * If an attempt to acquire the distributed lock fails and the migration is abandoned, the error
     * Status is placed in "migrationStatuses".
     */
    bool _takeDistLockForAMigration(OperationContext* txn,
                                    const Migration& migration,
                                    MigrationStatuses* migrationStatuses);

    /**
     * Attempts to acquire the distributed collection lock necessary required for "migration".
     */
    StatusWith<DistLockManager::ScopedDistLock> _getDistLock(OperationContext* txn,
                                                             const Migration& migration);

    // Protects class variables when migrations run in parallel.
    stdx::mutex _mutex;

    // Holds information about each ongoing migration.
    std::vector<Migration> _activeMigrations;

    // Temporary container for migrations that must be rescheduled. After all of the
    // _activeMigrations are finished, this variable is used to reset _activeMigrations before
    // executing migrations again.
    std::vector<Migration> _rescheduledMigrations;

    // Manages the distributed locks and whether the balancer or shard holds them.
    std::map<std::string, DistLockTracker> _distributedLocks;
};

}  // namespace mongo
