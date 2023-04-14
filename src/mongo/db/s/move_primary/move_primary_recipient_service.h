/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/s/move_primary/move_primary_recipient_cmds_gen.h"
#include "mongo/db/s/resharding/resharding_future_util.h"
#include "mongo/executor/scoped_task_executor.h"
#include "mongo/s/async_requests_sender.h"
#include <boost/optional.hpp>
#include <memory>

#include "mongo/client/fetcher.h"
#include "mongo/db/cloner.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/repl/oplog_fetcher.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/s/move_primary/move_primary_state_machine_gen.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/util/time_support.h"

namespace mongo {

class DBClientConnection;
class OperationContext;
class ReplicaSetMonitor;
class ServiceContext;

class MovePrimaryRecipientExternalState {
public:
    virtual ~MovePrimaryRecipientExternalState() = default;

    virtual std::vector<AsyncRequestsSender::Response> sendCommandToShards(
        OperationContext* opCtx,
        StringData dbName,
        const BSONObj& command,
        const std::vector<ShardId>& shardIds,
        const std::shared_ptr<executor::TaskExecutor>& executor) = 0;
};

class MovePrimaryRecipientExternalStateImpl final : public MovePrimaryRecipientExternalState {
public:
    std::vector<AsyncRequestsSender::Response> sendCommandToShards(
        OperationContext* opCtx,
        StringData dbName,
        const BSONObj& command,
        const std::vector<ShardId>& shardIds,
        const std::shared_ptr<executor::TaskExecutor>& executor) override;
};

class RecipientCancellationTokenHolder {
public:
    RecipientCancellationTokenHolder(CancellationToken stepdownToken)
        : _stepdownToken(stepdownToken),
          _abortSource(CancellationSource(stepdownToken)),
          _abortToken(_abortSource.token()) {}

    /**
     * Returns whether any token has been canceled.
     */
    bool isCanceled() {
        return _stepdownToken.isCanceled() || _abortToken.isCanceled();
    }

    /**
     * Returns true if an abort was triggered by user or if the recipient decided to abort the
     * operation.
     */
    bool isAborted() {
        return !_stepdownToken.isCanceled() && _abortToken.isCanceled();
    }

    /**
     * Returns whether the stepdownToken has been canceled, indicating that the shard's underlying
     * replica set node is stepping down or shutting down.
     */
    bool isSteppingOrShuttingDown() {
        return _stepdownToken.isCanceled();
    }

    /**
     * Cancels the source created by this class, in order to indicate to holders of the abortToken
     * that the movePrimary operation has been aborted.
     */
    void abort() {
        _abortSource.cancel();
    }

    const CancellationToken& getStepdownToken() {
        return _stepdownToken;
    }

    const CancellationToken& getAbortToken() {
        return _abortToken;
    }

private:
    // The token passed in by the PrimaryOnlyService runner that is canceled when this shard's
    // underlying replica set node is stepping down or shutting down.
    CancellationToken _stepdownToken;

    // The source created by inheriting from the stepdown token.
    CancellationSource _abortSource;

    // The token to wait on in cases where a user wants to wait on either a movePrimary operation
    // being aborted or the replica set node stepping/shutting down.
    CancellationToken _abortToken;
};

/**
 * MovePrimaryRecipientService coordinates online movePrimary data migration on the
 * recipient side.
 */
class MovePrimaryRecipientService : public repl::PrimaryOnlyService {
    // Disallows copying.
    MovePrimaryRecipientService(const MovePrimaryRecipientService&) = delete;
    MovePrimaryRecipientService& operator=(const MovePrimaryRecipientService&) = delete;

public:
    static constexpr StringData kMovePrimaryRecipientServiceName = "MovePrimaryRecipientService"_sd;

    explicit MovePrimaryRecipientService(ServiceContext* serviceContext);
    ~MovePrimaryRecipientService() = default;

    StringData getServiceName() const override;

    NamespaceString getStateDocumentsNS() const override {
        return NamespaceString::kMovePrimaryRecipientNamespace;
    }

    ThreadPool::Limits getThreadPoolLimits() const final;

    void checkIfConflictsWithOtherInstances(
        OperationContext* opCtx,
        BSONObj initialStateDoc,
        const std::vector<const PrimaryOnlyService::Instance*>& existingInstances) final;

    std::shared_ptr<PrimaryOnlyService::Instance> constructInstance(BSONObj initialStateDoc);

    class MovePrimaryRecipient final
        : public PrimaryOnlyService::TypedInstance<MovePrimaryRecipient> {
    public:
        explicit MovePrimaryRecipient(
            const MovePrimaryRecipientService* recipientService,
            MovePrimaryRecipientDocument recipientDoc,
            std::shared_ptr<MovePrimaryRecipientExternalState> externalState,
            ServiceContext* serviceContext,
            std::unique_ptr<Cloner> cloner);

        SemiFuture<void> run(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                             const CancellationToken& token) noexcept final;

        /**
         * This service relies on the stepdown token passed to run method of base class and hence
         * ignores the interrupts.
         */
        void interrupt(Status status) override{};

        /**
         * Aborts the ongoing movePrimary operation which should be user initiated.
         */
        void abort();

        /**
         * Returns a Future that will be resolved when _recipientDocDurablePromise is fulfilled.
         */
        SharedSemiFuture<void> getRecipientDocDurableFuture() const {
            return _recipientDocDurablePromise.getFuture();
        }

        /**
         * Returns a Future that will be resolved when the _dataClonePromise is fulfilled.
         */
        SharedSemiFuture<void> getDataClonedFuture() const {
            return _dataClonePromise.getFuture();
        }

        /**
         * Returns a Future that will be resolved when the recipient instance finishes movePrimary
         * op.
         */
        SharedSemiFuture<void> getCompletionFuture() const {
            return _completionPromise.getFuture();
        }

        /**
         * Fulfills _forgetMigrationPromise and returns future from _completionPromise.
         */
        SharedSemiFuture<void> onReceiveForgetMigration();

        /**
         * Returns Future that will be resolved when the _preparedPromise is fulfilled.
         */
        SharedSemiFuture<void> onReceiveSyncData(Timestamp blockTimestamp);

        /**
         * Report MovePrimaryRecipientService Instances in currentOp().
         */
        boost::optional<BSONObj> reportForCurrentOp(
            MongoProcessInterface::CurrentOpConnectionsMode connMode,
            MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept final;

        void checkIfOptionsConflict(const BSONObj& stateDoc) const final;

        NamespaceString getDatabaseName() const;

        UUID getMigrationId() const;

    private:
        ExecutorFuture<void> _transitionToInitializingState(
            const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

        ExecutorFuture<void> _transitionToCloningStateAndClone(
            const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

        ExecutorFuture<void> _initializeForCloningState(
            const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

        ExecutorFuture<void> _transitionToApplyingState(
            const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

        ExecutorFuture<void> _transitionToBlockingStateAndAcquireCriticalSection(
            const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

        ExecutorFuture<void> _transitionToPreparedState(
            const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

        ExecutorFuture<void> _transitionToAbortedStateAndCleanupOrphanedData(
            const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

        ExecutorFuture<void> _transitionToDoneStateAndFinishMovePrimaryOp(
            const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

        /**
         * This function is called if the recipient service decides to abort due to unrecoverable
         * errors.
         */
        void _internalAbort();

        /**
         * Clears cached database info on recipient shard to trigger a refresh on next request with
         * DB version. This is done before releasing critical section.
         */
        void _clearDatabaseMetadata(OperationContext* opCtx);

        void _createMetadataCollection(OperationContext* opCtx);

        std::vector<NamespaceString> _getUnshardedCollections(OperationContext* opCtx);

        void _persistCollectionsToClone(OperationContext* opCtx);

        std::vector<NamespaceString> _getCollectionsToClone(OperationContext* opCtx) const;

        void _cleanUpOrphanedDataOnRecipient(OperationContext* opCtx);

        void _cleanUpOperationMetadata(
            OperationContext* opCtx, const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

        void _removeRecipientDocument(OperationContext* opCtx);

        void _ensureUnfulfilledPromisesError(Status status);

        std::vector<NamespaceString> _getShardedCollectionsFromConfigSvr(
            OperationContext* opCtx) const;

        void _transitionStateMachine(MovePrimaryRecipientStateEnum newState);

        template <class T>
        void _updateRecipientDocument(OperationContext* opCtx,
                                      const StringData& fieldName,
                                      T value);

        repl::OpTime _getStartApplyingDonorOpTime(
            OperationContext* opCtx, const std::shared_ptr<executor::ScopedTaskExecutor>& executor);

        bool _checkInvalidStateTransition(MovePrimaryRecipientStateEnum newState);

        bool _canAbort(WithLock) const;

        bool _useOnlineCloner() const;

        void _cloneDataFromDonor(OperationContext* opCtx);

        NamespaceString _getCollectionsToCloneNSS() const;
        /**
         * Waits for majority write concern for client's last applied opTime. Cancels on stepDown.
         * This is needed after each state transition completes in future chain because disk updates
         * are done with kLocalWriteConcern in the _retryingCancelableOpCtxFactory retry loops.
         */
        ExecutorFuture<void> _waitForMajority(
            std::shared_ptr<executor::ScopedTaskExecutor> executor);

        const NamespaceString _stateDocumentNS = NamespaceString::kMovePrimaryRecipientNamespace;

        const MovePrimaryRecipientService* _recipientService;

        const MovePrimaryCommonMetadata _metadata;

        std::shared_ptr<MovePrimaryRecipientExternalState> _movePrimaryRecipientExternalState;

        ServiceContext* _serviceContext;

        // ThreadPool used by CancelableOperationContext.
        // CancelableOperationContext must have a thread that is always available to it to mark its
        // opCtx as killed when the cancelToken has been cancelled.
        const std::shared_ptr<ThreadPool> _markKilledExecutor;
        boost::optional<resharding::RetryingCancelableOperationContextFactory>
            _retryingCancelableOpCtxFactory;

        boost::optional<repl::OpTime> _startApplyingDonorOpTime;

        std::vector<NamespaceString> _shardedColls;

        const BSONObj _criticalSectionReason;

        const bool _resumedAfterFailover;

        // To synchronize operations on mutable states below.
        Mutex _mutex = MONGO_MAKE_LATCH("MovePrimaryRecipient::_mutex");

        // Used to catch the case when abort is called from a different thread around the time run()
        // is called.
        bool _abortCalled{false};

        // Holds the cancellation tokens relevant to the MovePrimaryRecipientService.
        std::unique_ptr<RecipientCancellationTokenHolder> _ctHolder;

        MovePrimaryRecipientStateEnum _state;

        std::unique_ptr<Cloner> _cloner;

        // Promise that is resolved when the recipient doc is persisted on disk
        SharedPromise<void> _recipientDocDurablePromise;

        // Promise that is resolved when the recipient successfully clones documents and transitions
        // to kApplying state.
        SharedPromise<void> _dataClonePromise;

        // Promise that is resolved when the recipient successfully applies oplog entries till
        // blockTimestamp from donor and enters kPrepared state
        SharedPromise<void> _preparedPromise;

        // Promise that is resolved when the recipient receives movePrimaryRecipientForgetMigration.
        SharedPromise<void> _forgetMigrationPromise;

        // Promise that is resolved when all the needed work for movePrimary op is completed at the
        // recipient for a successful or unsuccessful operation both.
        SharedPromise<void> _completionPromise;
    };

protected:
    static constexpr StringData movePrimaryOpLogBufferPrefix = "movePrimaryOplogBuffer"_sd;
    ServiceContext* const _serviceContext;
};

}  // namespace mongo
