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

#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/move_primary/move_primary_metrics.h"
#include "mongo/db/s/move_primary/move_primary_state_machine_gen.h"
#include "mongo/db/s/resharding/resharding_future_util.h"
#include "mongo/s/client/shard.h"

namespace mongo {

struct MovePrimaryDonorDependencies;

class MovePrimaryDonorService : public repl::PrimaryOnlyService {
public:
    static constexpr StringData kServiceName = "MovePrimaryDonorService"_sd;

    MovePrimaryDonorService(ServiceContext* serviceContext);

    StringData getServiceName() const override;

    NamespaceString getStateDocumentsNS() const override;

    ThreadPool::Limits getThreadPoolLimits() const override;

    void checkIfConflictsWithOtherInstances(
        OperationContext* opCtx,
        BSONObj initialState,
        const std::vector<const Instance*>& existingInstances) override;

    std::shared_ptr<PrimaryOnlyService::Instance> constructInstance(BSONObj initialState) override;

protected:
    virtual MovePrimaryDonorDependencies _makeDependencies(
        const MovePrimaryDonorDocument& initialDoc);

private:
    ServiceContext* _serviceContext;
};

class MovePrimaryDonorCancelState {
public:
    MovePrimaryDonorCancelState(const CancellationToken& stepdownToken);
    const CancellationToken& getStepdownToken();
    const CancellationToken& getAbortToken();
    bool isSteppingDown() const;
    void abort();

private:
    CancellationToken _stepdownToken;
    CancellationSource _abortSource;
    CancellationToken _abortToken;
};

// Retries indefinitely unless this node is stepping down. Intended to be used with
// resharding::RetryingCancelableOperationContextFactory for cases where an operation failure is not
// allowed to abort the task being performed by a PrimaryOnlyService (e.g. because that
// task is already aborted, or past the point where aborts are allowed).
template <typename BodyCallable>
class [[nodiscard]] IndefiniteRetryProvider {
public:
    explicit IndefiniteRetryProvider(BodyCallable&& body) : _body{std::move(body)} {}

    template <typename Predicate>
    auto until(Predicate&& predicate) && {
        using StatusType =
            decltype(std::declval<Future<FutureContinuationResult<BodyCallable>>>().getNoThrow());
        static_assert(
            std::is_invocable_r_v<bool, Predicate, StatusType>,
            "Predicate to until() must implement call operator accepting Status or StatusWith "
            "type that would be returned by this class's BodyCallable, and must return bool");
        return AsyncTry(std::move(_body))
            .until([predicate = std::move(predicate)](const auto& statusLike) {
                return predicate(statusLike);
            });
    }

private:
    BodyCallable _body;
};
class MovePrimaryDonorRetryHelper {
public:
    MovePrimaryDonorRetryHelper(const MovePrimaryCommonMetadata& metadata,
                                std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                MovePrimaryDonorCancelState* cancelState);

    template <typename Fn>
    auto untilStepdownOrMajorityCommit(const std::string& operationName, Fn&& fn) {
        return _untilStepdownOrSuccess(operationName, std::forward<Fn>(fn))
            .then([this, operationName] { return _waitForMajorityOrStepdown(operationName); });
    }

    template <typename Fn>
    auto untilAbortOrMajorityCommit(const std::string& operationName, Fn&& fn) {
        return _untilAbortOrSuccess(operationName, std::forward<Fn>(fn))
            .then([this, operationName] { return _waitForMajorityOrStepdown(operationName); });
    }

private:
    template <typename Fn>
    auto _untilStepdownOrSuccess(const std::string& operationName, Fn&& fn) {
        return _cancelOnStepdownFactory
            .withAutomaticRetry<decltype(fn), IndefiniteRetryProvider>(std::forward<Fn>(fn))
            .until([this, operationName](const auto& statusLike) {
                if (!statusLike.isOK()) {
                    _handleTransientError(operationName, statusLike);
                }
                return statusLike.isOK();
            })
            .withBackoffBetweenIterations(kBackoff)
            .on(**_taskExecutor, _cancelState->getStepdownToken());
    }

    template <typename Fn>
    auto _untilAbortOrSuccess(const std::string& operationName, Fn&& fn) {
        using FuturizedResultType =
            FutureContinuationResult<Fn, const CancelableOperationContextFactory&>;
        using StatusifiedResultType =
            decltype(std::declval<Future<FuturizedResultType>>().getNoThrow());
        return _cancelOnAbortFactory.withAutomaticRetry(std::forward<Fn>(fn))
            .onTransientError([this, operationName](const Status& status) {
                _handleTransientError(operationName, status);
            })
            .onUnrecoverableError([this, operationName](const Status& status) {
                _handleUnrecoverableError(operationName, status);
            })
            .template until<StatusifiedResultType>(
                [](const auto& statusLike) { return statusLike.isOK(); })
            .withBackoffBetweenIterations(kBackoff)
            .on(**_taskExecutor, _cancelState->getAbortToken());
    }

    void _handleTransientError(const std::string& operationName, const Status& status);
    void _handleUnrecoverableError(const std::string& operationName, const Status& status);
    ExecutorFuture<void> _waitForMajorityOrStepdown(const std::string& operationName);

    const static Backoff kBackoff;

    const MovePrimaryCommonMetadata _metadata;
    std::shared_ptr<executor::ScopedTaskExecutor> _taskExecutor;
    std::shared_ptr<ThreadPool> _markKilledExecutor;
    MovePrimaryDonorCancelState* _cancelState;
    resharding::RetryingCancelableOperationContextFactory _cancelOnStepdownFactory;
    resharding::RetryingCancelableOperationContextFactory _cancelOnAbortFactory;
};

class MovePrimaryDonorExternalState {
public:
    MovePrimaryDonorExternalState(const MovePrimaryCommonMetadata& metadata);
    virtual ~MovePrimaryDonorExternalState() = default;

    void syncDataOnRecipient(OperationContext* opCtx);
    void syncDataOnRecipient(OperationContext* opCtx, boost::optional<Timestamp> timestamp);
    void abortMigrationOnRecipient(OperationContext* opCtx);
    void forgetMigrationOnRecipient(OperationContext* opCtx);

protected:
    virtual StatusWith<Shard::CommandResponse> runCommand(OperationContext* opCtx,
                                                          const ShardId& shardId,
                                                          const ReadPreferenceSetting& readPref,
                                                          const std::string& dbName,
                                                          const BSONObj& cmdObj,
                                                          Shard::RetryPolicy retryPolicy) = 0;

    const MovePrimaryCommonMetadata& getMetadata() const;
    ShardId getRecipientShardId() const;

private:
    void _runCommandOnRecipient(OperationContext* opCtx, const BSONObj& command);

    MovePrimaryCommonMetadata _metadata;
};

class MovePrimaryDonorExternalStateImpl : public MovePrimaryDonorExternalState {
public:
    MovePrimaryDonorExternalStateImpl(const MovePrimaryCommonMetadata& metadata);

protected:
    StatusWith<Shard::CommandResponse> runCommand(OperationContext* opCtx,
                                                  const ShardId& shardId,
                                                  const ReadPreferenceSetting& readPref,
                                                  const std::string& dbName,
                                                  const BSONObj& cmdObj,
                                                  Shard::RetryPolicy retryPolicy) override;
};

struct MovePrimaryDonorDependencies {
    std::unique_ptr<MovePrimaryDonorExternalState> externalState;
};

class MovePrimaryDonor : public repl::PrimaryOnlyService::TypedInstance<MovePrimaryDonor> {
public:
    MovePrimaryDonor(ServiceContext* serviceContext,
                     MovePrimaryDonorService* donorService,
                     MovePrimaryDonorDocument initialState,
                     const std::shared_ptr<executor::TaskExecutor>& cleanupExecutor,
                     MovePrimaryDonorDependencies dependencies);

    SemiFuture<void> run(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                         const CancellationToken& stepdownToken) noexcept override;

    void interrupt(Status status) override;

    boost::optional<BSONObj> reportForCurrentOp(
        MongoProcessInterface::CurrentOpConnectionsMode connMode,
        MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept override;

    void checkIfOptionsConflict(const BSONObj& stateDoc) const override;

    const MovePrimaryCommonMetadata& getMetadata() const;

    void onBeganBlockingWrites(StatusWith<Timestamp> blockingWritesTimestamp);
    void onReadyToForget();

    void abort(Status reason);
    bool isAborted() const;

    SharedSemiFuture<void> getReadyToBlockWritesFuture() const;
    SharedSemiFuture<void> getDecisionFuture() const;
    SharedSemiFuture<void> getCompletionFuture() const;

private:
    MovePrimaryDonorStateEnum _getCurrentState() const;
    MovePrimaryDonorMutableFields _getMutableFields() const;
    bool _isAborted(WithLock) const;
    boost::optional<Status> _getAbortReason() const;
    Status _getOperationStatus() const;
    MovePrimaryDonorDocument _buildCurrentStateDocument() const;

    void _initializeRun(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                        const CancellationToken& stepdownToken);
    ExecutorFuture<void> _runDonorWorkflow();
    ExecutorFuture<void> _transitionToState(MovePrimaryDonorStateEnum state);
    ExecutorFuture<void> _doNothing();
    ExecutorFuture<void> _doInitializing();
    ExecutorFuture<void> _doCloning();
    ExecutorFuture<void> _doWaitingToBlockWrites();
    ExecutorFuture<void> _doBlockingWrites();
    ExecutorFuture<void> _waitUntilReadyToBlockWrites();
    ExecutorFuture<Timestamp> _waitUntilCurrentlyBlockingWrites();
    ExecutorFuture<void> _persistBlockingWritesTimestamp(Timestamp blockingWritesTimestamp);
    ExecutorFuture<void> _doPrepared();
    ExecutorFuture<void> _waitForForgetThenDoCleanup();
    ExecutorFuture<void> _doCleanup();
    ExecutorFuture<void> _doAbortIfRequired();
    ExecutorFuture<void> _ensureAbortReasonSetInStateDocument();
    ExecutorFuture<void> _doAbort();
    ExecutorFuture<void> _doForget();
    bool _allowedToAbortDuringStateTransition(MovePrimaryDonorStateEnum newState) const;
    void _tryTransitionToStateOnce(OperationContext* opCtx, MovePrimaryDonorStateEnum newState);
    void _updateOnDiskState(OperationContext* opCtx,
                            const MovePrimaryDonorDocument& newStateDocument);
    void _updateInMemoryState(const MovePrimaryDonorDocument& newStateDocument);
    void _ensureProgressPromisesAreFulfilled(Status result);

    template <typename Fn>
    auto _runOnTaskExecutor(Fn&& fn) {
        return ExecutorFuture(**_taskExecutor).then(std::forward<Fn>(fn));
    }

    mutable Mutex _mutex = MONGO_MAKE_LATCH("MovePrimaryDonor::_mutex");
    ServiceContext* _serviceContext;
    MovePrimaryDonorService* const _donorService;
    const MovePrimaryCommonMetadata _metadata;

    boost::optional<Status> _abortReason;
    MovePrimaryDonorMutableFields _mutableFields;
    std::unique_ptr<MovePrimaryMetrics> _metrics;

    std::shared_ptr<executor::TaskExecutor> _cleanupExecutor;
    std::shared_ptr<executor::ScopedTaskExecutor> _taskExecutor;
    boost::optional<MovePrimaryDonorCancelState> _cancelState;
    boost::optional<MovePrimaryDonorRetryHelper> _retry;

    std::unique_ptr<MovePrimaryDonorExternalState> _externalState;

    SharedPromise<void> _progressedToReadyToBlockWritesPromise;
    SharedPromise<void> _progressedToDecisionPromise;

    SharedPromise<Timestamp> _currentlyBlockingWritesPromise;
    SharedPromise<void> _readyToForgetPromise;

    SharedPromise<void> _completionPromise;
};

}  // namespace mongo
