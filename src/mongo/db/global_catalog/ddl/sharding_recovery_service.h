// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/service_context.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/util/modules.h"

#include <functional>
#include <set>
#include <string>

namespace mongo {

class [[MONGO_MOD_NEEDS_REPLACEMENT]] ShardingRecoveryService
    : public ReplicaSetAwareServiceShardSvr<ShardingRecoveryService> {

public:
    /*
     * Base interface for defining a preliminary action (in form of functor) to be executed by the
     * ShardingRecoveryService prior to the release of a Critical Section.
     */
    class BeforeReleasingCustomAction {

    public:
        virtual ~BeforeReleasingCustomAction() {}
        /*
         * The function encapsulating the custom action. Subclasses must ensure that its
         * implementation:
         * - is idemptotent
         * - is compatible with an execution context where the collection/database lock
         * for the namespace being released is already acquired in exclusive mode.
         */
        virtual void operator()(OperationContext*, const NamespaceString&) const = 0;
    };

    /*
     * Implementation for an empty custom action.
     */
    class NoCustomAction : public BeforeReleasingCustomAction {
    public:
        void operator()(OperationContext*, const NamespaceString&) const final {};
    };


    /*
     * Custom action for filtering metadata clearing.
     */
    class FilteringMetadataClearer : public BeforeReleasingCustomAction {
    public:
        FilteringMetadataClearer(bool includeStepsForNamespaceDropped = false);

        void operator()(OperationContext* opCtx,
                        const NamespaceString& nssBeingReleased) const final;

    private:
        const bool _includeStepsForNamespaceDropped;
    };


    ShardingRecoveryService() = default;

    static ShardingRecoveryService* get(ServiceContext* serviceContext);
    static ShardingRecoveryService* get(OperationContext* opCtx);

    /**
     * Callback type invoked at critical section lock acquisition if there is lock contention.
     * This allows callers to perform actions (e.g. killing unprepared transactions) while the lock
     * request is in the queue.
     */
    using CriticalSectionLockContendAction = std::function<void(OperationContext*)>;

    /**
     * Acquires the recoverable critical section in the catch-up phase (i.e. blocking writes) for
     * the specified namespace and reason. It works even if the namespace's current metadata are
     * UNKNOWN.
     *
     * Entering into the critical section interrupts any ongoing filtering metadata refresh.
     *
     * It adds a doc to `config.collectionCriticalSections` with with `writeConcern` write concern.
     *
     * Do nothing if the critical section is taken for that namespace and reason, and will invariant
     * otherwise since it is the responsibility of the caller to ensure that only one thread is
     * taking the critical section.
     *
     * The 'lockAcquisitionTimeout' parameter, when set, limits how much time this method will wait
     * for db/collection lock acquisition. If unable to acquire the locks within the specified time
     * limit, then a LockTimeout exception is thrown.
     *
     * NOTE: If the `clearShardCatalogCache` flag is set, at the time of releasing the
     * critical section it will also clear the filtering metadata. This flag is only used by
     * secondary nodes.
     */
    void acquireRecoverableCriticalSectionBlockWrites(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const BSONObj& reason,
        const WriteConcernOptions& writeConcern,
        bool clearShardCatalogCache,
        boost::optional<Milliseconds> lockAcquisitionTimeout = boost::none,
        const CriticalSectionLockContendAction& criticalSectionLockContendAction = nullptr);

    /**
     * Advances the recoverable critical section from the catch-up phase (i.e. blocking writes) to
     * the commit phase (i.e. blocking reads) for the specified namespace and reason. The
     * recoverable critical section must have been acquired first through
     * `acquireRecoverableCriticalSectionBlockWrites` function.
     *
     * It updates a doc from `config.collectionCriticalSections` with `writeConcern` write concern.
     *
     * Do nothing if the critical section is already taken in commit phase.
     *
     * The 'lockAcquisitionTimeout' parameter, when set, limits how much time this method will wait
     * for db/collection lock acquisition. If unable to acquire the locks within the specified time
     * limit, then a LockTimeout exception is thrown.
     */
    void promoteRecoverableCriticalSectionToBlockAlsoReads(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const BSONObj& reason,
        const WriteConcernOptions& writeConcern,
        boost::optional<Milliseconds> lockAcquisitionTimeout = boost::none);

    /**
     * Releases the recoverable critical section for the given namespace and reason.
     *
     * It removes a doc from `config.collectionCriticalSections` with `writeConcern` write concern.
     *
     * Does nothing if the critical section is not actually taken for that namespace and reason.
     *
     * The execution of a custom action immediately prior to the release may be specified by the
     * caller through the related parameter (see comments on BeforeReleasingCustomAction and its
     * subclasses for more details).
     *
     * Invariants in case the collection critical section is already taken by another operation with
     * a different reason unless the flag 'throwIfReasonDiffers' is set to false.
     */
    void releaseRecoverableCriticalSection(OperationContext* opCtx,
                                           const NamespaceString& nss,
                                           const BSONObj& reason,
                                           const WriteConcernOptions& writeConcern,
                                           const BeforeReleasingCustomAction& beforeReleasingAction,
                                           bool throwIfReasonDiffers = true);

    /**
     * Returns true if a recoverable critical section is currently held for the given namespace with
     * the exact given reason, false otherwise.
     */
    bool isCriticalSectionHeld(OperationContext* opCtx,
                               const NamespaceString& nss,
                               const BSONObj& reason);

    /**
     * Recovers the in-memory sharding state from disk in case of rollback.
     */
    void onReplicationRollback(OperationContext* opCtx,
                               const std::set<NamespaceString>& rollbackNamespaces);

    /**
     * Recovers the in-memory sharding state from disk when either initial sync or startup recovery
     * have completed.
     */
    void onConsistentDataAvailable(OperationContext* opCtx, bool isMajority, bool isRollback) final;

private:
    /**
     * This method is called to reset the states before recover (mirror the state on disk to
     * memory). It must be called before the recover functions.
     */
    void _resetInMemoryStates(OperationContext* opCtx);

    /**
     * This method is called when we have to mirror the state on disk of the recoverable critical
     * section to memory (on startup or on rollback).
     *
     * NOTE: It must be called after `_recoverDatabaseShardingState`.
     */
    void _recoverRecoverableCriticalSections(OperationContext* opCtx);

    /**
     * This method is called when we have to mirror the state on disk of the database sharding state
     * to memory (on startup or on rollback).
     */
    void _recoverDatabaseShardingState(OperationContext* opCtx);

    /**
     * This method is called when we have to recover the allowChunkOperations flag from disk to
     * memory (on startup or on rollback).
     */
    void _recoverAllowChunkOperations(OperationContext* opCtx);

    void onStartup(OperationContext* opCtx) final {}
    void onSetCurrentConfig(OperationContext* opCtx) final {}
    void onShutdown() final {}
    void onStepUpBegin(OperationContext* opCtx, long long term) final {}
    void onStepUpComplete(OperationContext* opCtx, long long term) final {}
    void onStepDown() final {}
    void onRollbackBegin() final {}
    void onBecomeArbiter() final {}
    inline std::string getServiceName() const final {
        return "ShardingRecoveryService";
    }
};

}  // namespace mongo
