/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replica_set_aware_service.h"
#include "mongo/db/service_context.h"
#include "mongo/db/write_concern_options.h"

#include <set>
#include <string>

namespace mongo {

class ShardingRecoveryService : public ReplicaSetAwareServiceShardSvr<ShardingRecoveryService> {

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
     * NOTE: If the `clearDbMetadata` flag is set, at the time of releasing the database critical
     * section it will also clear the filtering database information. This flag is only used by
     * secondary nodes.
     */
    void acquireRecoverableCriticalSectionBlockWrites(OperationContext* opCtx,
                                                      const NamespaceString& nss,
                                                      const BSONObj& reason,
                                                      const WriteConcernOptions& writeConcern,
                                                      bool clearDbMetadata = true);

    /**
     * Advances the recoverable critical section from the catch-up phase (i.e. blocking writes) to
     * the commit phase (i.e. blocking reads) for the specified namespace and reason. The
     * recoverable critical section must have been acquired first through
     * `acquireRecoverableCriticalSectionBlockWrites` function.
     *
     * It updates a doc from `config.collectionCriticalSections` with `writeConcern` write concern.
     *
     * Do nothing if the critical section is already taken in commit phase.
     */
    void promoteRecoverableCriticalSectionToBlockAlsoReads(OperationContext* opCtx,
                                                           const NamespaceString& nss,
                                                           const BSONObj& reason,
                                                           const WriteConcernOptions& writeConcern);

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
