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

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/s/request_types/move_range_request_gen.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

class OperationContext;

class ScopedDonateChunk;
class ScopedReceiveChunk;
class ScopedSplitMergeChunk;

namespace migrationutil {
void resumeMigrationRecipientsOnStepUp(OperationContext* opCtx);
}  // namespace migrationutil

/**
 * This class is used to synchronise all the active routing info operations for chunks owned by this
 * shard. There is only one instance of it per ServiceContext.
 *
 * It tracks three kinds of chunk operations running on this shard:
 *   - Donate (move): this shard is the source of a chunk migration.
 *   - Receive: this shard is the destination of a chunk migration.
 *   - Split/Merge: this shard is splitting or merging chunks it owns.
 *
 * Donate and Receive are mutually exclusive shard-wide: a shard performs at most one migration at a
 * time, as either donor or recipient, regardless of namespace. Split/Merge is tracked per
 * namespace and coordinates with the other operations only on the same namespace, so unrelated
 * collections can split/merge concurrently with each other and with a migration.
 *
 * When a new operation arrives while another is already running, the outcome is:
 *
 *   Legend:
 *     JOIN       - attaches to the running operation and shares its outcome
 *     CONFLICT   - fails immediately with ConflictingOperationInProgress
 *     WAIT       - blocks until the running operation finishes, then proceeds
 *     CONCURRENT - proceeds immediately; both run at the same time
 *     WAIT/CONFL - WAIT when registerReceiveChunk() is called with
 *                  waitForCompletionOfMigrationOps == true (e.g. the step-up recovery path);
 *                  CONFLICT otherwise.
 *
 *   Rows = incoming operation, Columns = operation already running on this shard.
 *
 *   incoming \ running | Donate (move)          | Receive               | Split/Merge
 *   -------------------+------------------------+-----------------------+----------------------
 *    Donate (move)     | same request: JOIN     | CONFLICT              | same nss:  WAIT
 *                      | otherwise:    CONFLICT |                       | other nss: CONCURRENT
 *   -------------------+------------------------+-----------------------+----------------------
 *    Receive           | WAIT/CONFL             | WAIT/CONFL            | same nss:  WAIT
 *                      |                        |                       | other nss: CONCURRENT
 *   -------------------+------------------------+-----------------------+----------------------
 *    Split/Merge       | same nss:  WAIT        | same nss:  WAIT       | same nss:  WAIT
 *                      | other nss: CONCURRENT  | other nss: CONCURRENT | other nss: CONCURRENT
 *
 * Migrations and Split/Merge of the same namespace wait for each other (whichever starts second
 * blocks until the first finishes), so the two never hold the collection critical section at the
 * same time.
 */
class MONGO_MOD_NEEDS_REPLACEMENT ActiveMigrationsRegistry {
    ActiveMigrationsRegistry(const ActiveMigrationsRegistry&) = delete;
    ActiveMigrationsRegistry& operator=(const ActiveMigrationsRegistry&) = delete;

public:
    ActiveMigrationsRegistry();
    ~ActiveMigrationsRegistry();

    static ActiveMigrationsRegistry& get(ServiceContext* service);
    static ActiveMigrationsRegistry& get(OperationContext* opCtx);

    /**
     * Interface injected to let the registry wait for the sharding coordinator service recovery
     * to complete before acquiring any of its locks. Any code path that acquires the
     * ActiveMigrationsRegistry must first await completion of the sharding coordinator's recovery,
     * so that recovered coordinators obtain their ActiveMigrationsRegistry before newly-submitted
     * operations.
     */
    class MONGO_MOD_OPEN Recoverable {
    public:
        virtual ~Recoverable() = default;

        virtual void waitForRecovery(OperationContext* opCtx) const = 0;
    };

    // Injects the Recoverable instance the registry will block on before acquiring any of its
    // locks. Expected to be called once per ServiceContext at shard-server startup.
    void setRecoverable(Recoverable* recoverable);

    /**
     * PassKey token that authorises a registry-lock acquisition to skip the waitForRecovery()
     * step. The friend list below is the complete set of sites that can construct one, each for
     * the same underlying reason — they run inside the recovery sequence and would otherwise
     * deadlock or fail by waiting on it:
     *
     *   - ChunkOperationShardingCoordinator<StateDoc> (the base template for chunk-operation
     *     coordinators), when a coordinator reacquires the registry from inside its own recovery /
     *     lock-acquisition phase. Waiting on the coordinator service's recovery from that context
     *     would deadlock, since the caller is itself part of what's holding recovery open. The
     *     template is the friend (rather than each concrete coordinator) so that the bypass token
     *     can only be obtained via the protected static helper
     *     `ChunkOperationShardingCoordinator::makeRegistryRecoveryBypass()`, which is the single
     *     sanctioned construction site for the token within the coordinator hierarchy.
     *   - migrationutil::resumeMigrationRecipientsOnStepUp, which executes synchronously on the
     *     OplogApplier thread during step-up — before ShardingCoordinatorService has transitioned
     *     out of kPaused. Without the bypass, waitForRecovery would uassert NotWritablePrimary and
     *     crash the node.
     *   - ActiveMigrationsRegistryTestAccessor, which exists only in the unit test to exercise
     *     recovery-bypassed registration behavior.
     *
     * Because the default constructor is private and the friend list above is the only way to
     * reach it, code outside these surfaces cannot bypass the recovery wait.
     */
    class BypassRecoveryWait {
    private:
        template <typename StateDoc>
        friend class ChunkOperationShardingCoordinator;
        friend class ActiveMigrationsRegistryTestAccessor;
        friend void migrationutil::resumeMigrationRecipientsOnStepUp(OperationContext*);
        BypassRecoveryWait() = default;
    };

    /**
     * These methods can be used to block all chunk operations (migrations as well as split/merge
     * operations) temporarily. The lock() method will block while there is any chunk operation in
     * progress and will return once they have all completed. Any subsequent chunk operation will
     * return ConflictingOperationInProgress until the unlock() method is called.
     */
    void lock(OperationContext* opCtx,
              std::string_view reason,
              boost::optional<BypassRecoveryWait> bypass = boost::none);
    void unlock(std::string_view reason);

    /**
     * If there are no migrations or split/merges running on this shard, registers an active
     * migration with the specified arguments. Returns a ScopedDonateChunk, which must be signaled
     * by the caller before it goes out of scope.
     *
     * If there is an active migration already running on this shard and it has the exact same
     * arguments, returns a ScopedDonateChunk. The ScopedDonateChunk can be used to join the
     * already running migration.
     *
     * Otherwise returns a ConflictingOperationInProgress error.
     */
    StatusWith<ScopedDonateChunk> registerDonateChunk(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const ShardsvrMoveRangeRequest& request,
        boost::optional<BypassRecoveryWait> bypass = boost::none);

    /**
     * Registers an active receive chunk operation with the specified session id and returns a
     * ScopedReceiveChunk. The returned ScopedReceiveChunk object will unregister the migration when
     * it goes out of scope.
     *
     * A receive always waits for any split/merge of the same namespace to finish before
     * registering, since both would take the collection critical section.
     *
     * For a conflicting migration (a second receive or an active donate) or an explicit registry
     * lock(), the function will either
     * - wait for such operations to complete and then perform the registration
     * - return a ConflictingOperationInProgress error
     * based on the value of the waitForCompletionOfMigrationOps parameter.
     */
    StatusWith<ScopedReceiveChunk> registerReceiveChunk(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const ChunkRange& chunkRange,
        const ShardId& fromShardId,
        bool waitForCompletionOfMigrationOps,
        boost::optional<BypassRecoveryWait> bypass = boost::none);

    /**
     * If there are no migrations running on this shard, registers an active split or merge
     * operation for the specified namespace and returns a scoped object which will in turn disallow
     * other migrations or splits/merges for the same namespace (but not for other namespaces).
     */
    StatusWith<ScopedSplitMergeChunk> registerSplitOrMergeChunk(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const ChunkRange& chunkRange,
        boost::optional<BypassRecoveryWait> bypass = boost::none);

    /**
     * If a migration has been previously registered through a call to registerDonateChunk, returns
     * that namespace. Otherwise returns boost::none.
     */
    boost::optional<NamespaceString> getActiveDonateChunkNss();

    /**
     * Returns a report on the active migration if there currently is one. Otherwise, returns an
     * empty BSONObj.
     *
     * Takes an IS lock on the namespace of the active migration, if one is active.
     */
    BSONObj getActiveMigrationStatusReport(OperationContext* opCtx);

private:
    friend class ScopedDonateChunk;
    friend class ScopedReceiveChunk;
    friend class ScopedSplitMergeChunk;

    // Describes the state of a currently active moveChunk operation
    struct ActiveMoveChunkState {
        ActiveMoveChunkState(NamespaceString inNss, ShardsvrMoveRangeRequest inRequest)
            : nss(std::move(inNss)),
              request(std::move(inRequest)),
              promise(std::make_shared<SharedPromise<void>>()) {}

        /**
         * Constructs an error status to return in the case of conflicting operations.
         */
        Status constructErrorStatus() const;

        // Namespace of the currently active operation
        NamespaceString nss;

        // Move-range request fields of the currently active operation.
        ShardsvrMoveRangeRequest request;

        // Promise that will be resolved when the currently active operation completes.
        std::shared_ptr<SharedPromise<void>> promise;
    };

    // Describes the state of a currently active receive chunk operation
    struct ActiveReceiveChunkState {
        ActiveReceiveChunkState(NamespaceString inNss, ChunkRange inRange, ShardId inFromShardId)
            : nss(std::move(inNss)), range(std::move(inRange)), fromShardId(inFromShardId) {}

        /**
         * Constructs an error status to return in the case of conflicting operations.
         */
        Status constructErrorStatus() const;

        // Namespace for which a chunk is being received
        NamespaceString nss;

        // Bounds of the chunk being migrated
        ChunkRange range;

        // Id of the shard from which the chunk is being received
        ShardId fromShardId;
    };

    // Describes the state of a currently active split or merge operation
    struct ActiveSplitMergeChunkState {
        ActiveSplitMergeChunkState(NamespaceString inNss, ChunkRange inRange)
            : nss(std::move(inNss)), range(std::move(inRange)) {}

        // Namespace for which a chunk is being split or merged
        NamespaceString nss;

        // If split, bounds of the chunk being split; if merge, the end bounds of the range being
        // merged
        ChunkRange range;
    };

    /**
     * Unregisters a previously registered namespace with an ongoing migration. Must only be called
     * if a previous call to registerDonateChunk has succeeded.
     */
    void _clearDonateChunk();

    /**
     * Unregisters a previously registered incoming migration. Must only be called if a previous
     * call to registerReceiveChunk has succeeded.
     */
    void _clearReceiveChunk();

    /**
     * Unregisters a previously registered split/merge chunk operation. Must only be called if a
     * previous call to registerSplitOrMergeChunk has succeeded.
     */
    void _clearSplitMergeChunk(const NamespaceString& nss);

    // Recoverable on which waitForRecovery() is invoked before acquiring any registry lock. Set
    // once at shard-server startup and never cleared; nullptr on non-shard-server contexts and
    // in unit tests that don't wire one up. Not owned.
    Recoverable* _recoverable{nullptr};

    // Protects the state below
    std::mutex _mutex;

    // Condition variable which will be signaled whenever any of the states below become false,
    // boost::none or a specific namespace removed from the map.
    stdx::condition_variable _chunkOperationsStateChangedCV;

    // Overarching block, which doesn't allow migrations to occur even when there isn't an active
    // migration ongoing. Used during recovery and FCV changes.
    bool _migrationsBlocked{false};

    // If there is an active moveChunk operation, this field contains the original request
    boost::optional<ActiveMoveChunkState> _activeMoveChunkState;

    // If there is an active chunk receive operation, this field contains the original session id
    boost::optional<ActiveReceiveChunkState> _activeReceiveChunkState;

    // If there is an active split or merge chunk operation for a particular namespace, this map
    // will contain an entry
    stdx::unordered_map<NamespaceString, ActiveSplitMergeChunkState> _activeSplitMergeChunkStates;
};

/**
 * RAII guard that quiesces chunk operations on this shard for as long as it is in scope. On
 * construction it acquires the ActiveMigrationsRegistry lock, which drains every in-progress chunk
 * operation (migrations as well as split/merge operations) and blocks until they complete; while
 * held, any newly submitted chunk operation fails with ConflictingOperationInProgress. The
 * destructor releases the lock and lets chunk operations resume.
 */
class MONGO_MOD_PUBLIC MigrationBlockingGuard {
public:
    MigrationBlockingGuard(OperationContext* opCtx, std::string reason)
        : _registry(ActiveMigrationsRegistry::get(opCtx)), _reason(std::move(reason)) {
        // Ensure any thread attempting to use a MigrationBlockingGuard will be interrupted by
        // a stepdown.
        invariant(
            shard_role_details::getLocker(opCtx)->wasGlobalLockTakenInModeConflictingWithWrites() ||
            opCtx->shouldAlwaysInterruptAtStepDownOrUp());
        _registry.lock(opCtx, _reason);
    }

    ~MigrationBlockingGuard() {
        _registry.unlock(_reason);
    }

private:
    ActiveMigrationsRegistry& _registry;
    std::string _reason;
};

/**
 * Object of this class is returned from the registerDonateChunk call of the active migrations
 * registry. It can exist in two modes - 'execute' and 'join'. See the comments for
 * registerDonateChunk method for more details.
 */
class ScopedDonateChunk {
    ScopedDonateChunk(const ScopedDonateChunk&) = delete;
    ScopedDonateChunk& operator=(const ScopedDonateChunk&) = delete;

public:
    ScopedDonateChunk(ActiveMigrationsRegistry* registry,
                      bool shouldExecute,
                      std::shared_ptr<SharedPromise<void>> promise);
    ~ScopedDonateChunk();

    ScopedDonateChunk(ScopedDonateChunk&&);
    ScopedDonateChunk& operator=(ScopedDonateChunk&&);

    /**
     * Returns true if the migration object is in the 'execute' mode. This means that the migration
     * object holder is next in line to execute the moveChunk command. The holder must execute the
     * command and call signalComplete with a status.
     */
    bool mustExecute() const {
        return _shouldExecute;
    }

    /**
     * Must only be called if the object is in the 'execute' mode when the moveChunk command was
     * invoked (the command immediately executed). Signals any callers that might be blocked in
     * waitForCompletion.
     */
    void signalComplete(Status status);

    /**
     * Must only be called if the object is in the 'join' mode. Blocks until the main executor of
     * the moveChunk command calls signalComplete.
     */
    Status waitForCompletion(OperationContext* opCtx);

private:
    // Registry from which to unregister the migration. Not owned.
    ActiveMigrationsRegistry* _registry;

    /**
     * Whether the holder is the first in line for a newly started migration (in which case the
     * destructor must unregister) or the caller is joining on an already-running migration
     * (in which case the caller must block and wait for completion).
     */
    bool _shouldExecute;

    std::shared_ptr<SharedPromise<void>> _promise;
};

/**
 * Object of this class is returned from the registerReceiveChunk call of the active migrations
 * registry.
 */
class ScopedReceiveChunk {
    ScopedReceiveChunk(const ScopedReceiveChunk&) = delete;
    ScopedReceiveChunk& operator=(const ScopedReceiveChunk&) = delete;

public:
    ScopedReceiveChunk(ActiveMigrationsRegistry* registry);
    ~ScopedReceiveChunk();

    ScopedReceiveChunk(ScopedReceiveChunk&&);
    ScopedReceiveChunk& operator=(ScopedReceiveChunk&&);

private:
    // Registry from which to unregister the migration. Not owned.
    ActiveMigrationsRegistry* _registry;
};

/**
 * Object of this class is returned from the registerSplitOrMergeChunk call of the active migrations
 * registry.
 */
class MONGO_MOD_PUBLIC ScopedSplitMergeChunk {
public:
    ScopedSplitMergeChunk(ActiveMigrationsRegistry* registry, const NamespaceString& nss);
    ~ScopedSplitMergeChunk();

    ScopedSplitMergeChunk(ScopedSplitMergeChunk&&);
    ScopedSplitMergeChunk& operator=(ScopedSplitMergeChunk&&);

private:
    // Registry from which to unregister the split/merge. Not owned.
    ActiveMigrationsRegistry* _registry;

    NamespaceString _nss;
};

}  // namespace mongo
