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

#include <list>
#include <map>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/balancer/balancer_policy.h"
#include "mongo/executor/task_executor.h"
#include "mongo/s/catalog/dist_lock_manager.h"
#include "mongo/s/migration_secondary_throttle_options.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/concurrency/notification.h"

namespace mongo {

class OperationContext;
class ScopedMigrationRequest;
class ServiceContext;
class Status;
template <typename T>
class StatusWith;

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
    MigrationManager(ServiceContext* serviceContext);
    ~MigrationManager();

    /**
     * A blocking method that attempts to schedule all the migrations specified in
     * "candidateMigrations" and wait for them to complete. Takes the distributed lock for each
     * collection with a chunk being migrated.
     *
     * If any of the migrations, which were scheduled in parallel fails with a LockBusy error
     * reported from the shard, retries it serially without the distributed lock.
     *
     * Returns a map of migration Status objects to indicate the success/failure of each migration.
     */
    MigrationStatuses executeMigrationsForAutoBalance(
        OperationContext* txn,
        const std::vector<MigrateInfo>& migrateInfos,
        uint64_t maxChunkSizeBytes,
        const MigrationSecondaryThrottleOptions& secondaryThrottle,
        bool waitForDelete);

    /**
     * A blocking method that attempts to schedule the migration specified in "migrateInfo" and
     * waits for it to complete. Takes the distributed lock for the namespace which is being
     * migrated.
     *
     * Returns the status of the migration.
     */
    Status executeManualMigration(OperationContext* txn,
                                  const MigrateInfo& migrateInfo,
                                  uint64_t maxChunkSizeBytes,
                                  const MigrationSecondaryThrottleOptions& secondaryThrottle,
                                  bool waitForDelete);

    /**
     * Non-blocking method that puts the migration manager in the kRecovering state, in which
     * new migration requests will block until finishRecovery is called. Then does local writes to
     * reacquire the distributed locks for active migrations.
     *
     * The active migration recovery may fail and be abandoned, setting the state to kEnabled.
     */
    void startRecoveryAndAcquireDistLocks(OperationContext* txn);

    /**
     * Blocking method that must only be called after startRecovery has been called. Recovers the
     * state of the migration manager (if necessary and able) and puts it in the kEnabled state,
     * where it will accept new migrations. Any migrations waiting on the recovery state will be
     * unblocked once the state is kEnabled, and then this function waits for the recovered active
     * migrations to finish before returning.
     *
     * The active migration recovery may fail and be abandoned, setting the state to kEnabled and
     * unblocking any process waiting on the recovery state.
     */
    void finishRecovery(OperationContext* txn,
                        uint64_t maxChunkSizeBytes,
                        const MigrationSecondaryThrottleOptions& secondaryThrottle,
                        bool waitForDelete);

    /**
     * Non-blocking method that should never be called concurrently with finishRecovery. Puts the
     * manager in a state where all subsequently scheduled migrations will immediately fail (without
     * ever getting scheduled) and all active ones will be cancelled. It has no effect if the
     * migration manager is already stopping or stopped.
     */
    void interruptAndDisableMigrations();

    /**
     * Blocking method that waits for any currently scheduled migrations to complete. Must be
     * called after interruptAndDisableMigrations has been called in order to be able to re-enable
     * migrations again.
     */
    void drainActiveMigrations();

private:
    // The current state of the migration manager
    enum class State {  // Allowed transitions:
        kStopped,       // kRecovering
        kRecovering,    // kEnabled, kStopping
        kEnabled,       // kStopping
        kStopping,      // kStopped
    };

    /**
     * Tracks the execution state of a single migration.
     */
    struct Migration {
        Migration(NamespaceString nss, BSONObj moveChunkCmdObj);
        ~Migration();

        // Namespace for which this migration applies
        NamespaceString nss;

        // Command object representing the migration
        BSONObj moveChunkCmdObj;

        // Callback handle for the migration network request. If the migration has not yet been sent
        // on the network, this value is not set.
        boost::optional<executor::TaskExecutor::CallbackHandle> callbackHandle;

        // Notification, which will be signaled when the migration completes
        std::shared_ptr<Notification<executor::RemoteCommandResponse>> completionNotification;
    };

    // Used as a type in which to store a list of active migrations. The reason to choose list is
    // that its iterators do not get invalidated when entries are removed around them. This allows
    // O(1) removal time.
    using MigrationsList = std::list<Migration>;

    /**
     * Contains the runtime state for a single collection. This class does not have concurrency
     * control of its own and relies on the migration manager's mutex.
     */
    struct CollectionMigrationsState {
        CollectionMigrationsState(DistLockHandle distLockHandle);
        ~CollectionMigrationsState();

        // Dist lock handle, which must be released at destruction time.
        const DistLockHandle distLockHandle;

        // Contains a set of migrations which are currently active for this namespace.
        MigrationsList migrations;
    };

    using CollectionMigrationsStateMap =
        stdx::unordered_map<NamespaceString, CollectionMigrationsState>;

    /**
     * Optionally takes the collection distributed lock and schedules a chunk migration with the
     * specified parameters. May block for distributed lock acquisition. If dist lock acquisition is
     * successful (or not done), schedules the migration request and returns a notification which
     * can be used to obtain the outcome of the operation.
     *
     * The 'shardTakesCollectionDistLock' parameter controls whether the distributed lock is
     * acquired by the migration manager or by the shard executing the migration request.
     */
    std::shared_ptr<Notification<executor::RemoteCommandResponse>> _schedule(
        OperationContext* txn,
        const MigrateInfo& migrateInfo,
        bool shardTakesCollectionDistLock,
        uint64_t maxChunkSizeBytes,
        const MigrationSecondaryThrottleOptions& secondaryThrottle,
        bool waitForDelete);

    /**
     * Acquires the collection distributed lock for the specified namespace and if it succeeds,
     * schedules the migration.
     *
     * The distributed lock is acquired before scheduling the first migration for the collection and
     * is only released when all active migrations on the collection have finished.
     */
    void _scheduleWithDistLock_inlock(OperationContext* txn,
                                      const HostAndPort& targetHost,
                                      Migration migration);

    /**
     * Used internally for migrations scheduled with the distributed lock acquired by the config
     * server. Called exactly once for each scheduled migration, it will signal the migration in the
     * passed iterator and if this is the last migration for the collection will free the collection
     * distributed lock.
     */
    void _completeWithDistLock_inlock(OperationContext* txn,
                                      MigrationsList::iterator itMigration,
                                      const executor::RemoteCommandResponse& remoteCommandResponse);

    /**
     * Immediately schedules the specified migration without attempting to acquire the collection
     * distributed lock or checking that it is not being held.
     *
     * This method is only used for retrying migrations that have failed with LockBusy errors
     * returned by the shard, which only happens with legacy 3.2 shards that take the collection
     * distributed lock themselves.
     */
    void _scheduleWithoutDistLock_inlock(OperationContext* txn,
                                         const HostAndPort& targetHost,
                                         Migration migration);

    /**
     * If the state of the migration manager is kStopping, checks whether there are any outstanding
     * scheduled requests and if there aren't any signals the class condition variable.
     */
    void _checkDrained_inlock();

    /**
     * Blocking call, which waits for the migration manager to leave the recovering state (if it is
     * currently recovering).
     */
    void _waitForRecovery();

    /**
     * Should only be called from startRecovery or finishRecovery functions when the migration
     * manager is in either the kStopped or kRecovering state. Releases all the distributed locks
     * that the balancer holds, clears the config.migrations collection, changes the state of the
     * migration manager to kEnabled. Then unblocks all processes waiting for kEnabled state.
     */
    void _abandonActiveMigrationsAndEnableManager(OperationContext* txn);

    /**
     * Parses a moveChunk RemoteCommandResponse's two levels of Status objects and distiguishes
     * between errors generated by this config server and the shard primary to which the moveChunk
     * command was sent.
     *
     * If the command failed because of stepdown of this config server, the migration document
     * managed by 'scopedMigrationRequest' is saved for later balancer recovery and a
     * BalancerInterrupted error is returned. If the command failed because the shard to which the
     * command was sent returned an error, the migration document is not saved and the error is
     * returned without conversion.
     */
    Status _processRemoteCommandResponse(
        const executor::RemoteCommandResponse& remoteCommandResponse,
        ScopedMigrationRequest* scopedMigrationRequest);

    // The service context under which this migration manager runs.
    ServiceContext* const _serviceContext;

    // Used as a constant session ID for all distributed locks that this MigrationManager holds.
    // Currently required so that locks can be reacquired for the balancer in startRecovery and then
    // overtaken in later operations.
    OID _lockSessionID;

    // Carries migration information over from startRecovery to finishRecovery. Should only be set
    // in startRecovery and then accessed in finishRecovery.
    stdx::unordered_map<NamespaceString, std::list<MigrateInfo>> _migrationRecoveryMap;

    // Protects the class state below.
    stdx::mutex _mutex;

    // Always start the migration manager in a stopped state.
    State _state{State::kStopped};

    // Condition variable, which is waited on when the migration manager's state is changing and
    // signaled when the state change is complete.
    stdx::condition_variable _condVar;

    // Holds information about each collection's distributed lock and active migrations via a
    // CollectionMigrationState object.
    CollectionMigrationsStateMap _activeMigrationsWithDistLock;

    // Holds information about migrations, which have been scheduled without the collection
    // distributed lock acquired (i.e., the shard is asked to acquire it).
    MigrationsList _activeMigrationsWithoutDistLock;
};

}  // namespace mongo
