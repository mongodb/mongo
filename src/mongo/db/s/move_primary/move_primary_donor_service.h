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
    virtual MovePrimaryDonorDependencies makeDependencies(
        const MovePrimaryDonorDocument& initialDoc);

private:
    ServiceContext* _serviceContext;
};

class MovePrimaryDonorCancelState {
public:
    MovePrimaryDonorCancelState(const CancellationToken& stepdownToken);
    const CancellationToken& getStepdownToken();
    const CancellationToken& getAbortToken();

private:
    CancellationToken _stepdownToken;
    CancellationSource _abortSource;
    CancellationToken _abortToken;
};

class MovePrimaryDonorRetryHelper {
public:
    MovePrimaryDonorRetryHelper(const MovePrimaryCommonMetadata& metadata,
                                std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                MovePrimaryDonorCancelState* cancelState);

    template <typename Fn>
    auto untilStepdownOrSuccess(const std::string& operationName, Fn&& fn) {
        return untilImpl(operationName,
                         std::forward<Fn>(fn),
                         _cancelOnStepdownFactory,
                         _cancelState->getStepdownToken());
    }

    template <typename Fn>
    auto untilAbortOrSuccess(const std::string& operationName, Fn&& fn) {
        return untilImpl(operationName,
                         std::forward<Fn>(fn),
                         _cancelOnAbortFactory,
                         _cancelState->getAbortToken());
    }

private:
    template <typename Fn>
    using FuturizedResultType =
        FutureContinuationResult<Fn, const CancelableOperationContextFactory&>;

    template <typename Fn>
    using StatusifiedResultType =
        decltype(std::declval<Future<FuturizedResultType<Fn>>>().getNoThrow());

    template <typename Fn>
    auto untilImpl(const std::string& operationName,
                   Fn&& fn,
                   const resharding::RetryingCancelableOperationContextFactory& factory,
                   const CancellationToken& token) {
        return factory.withAutomaticRetry(std::forward<Fn>(fn))
            .onTransientError([this, operationName](const Status& status) {
                handleTransientError(operationName, status);
            })
            .onUnrecoverableError([this, operationName](const Status& status) {
                handleUnrecoverableError(operationName, status);
            })
            .template until<StatusifiedResultType<Fn>>(
                [](const auto& statusLike) { return statusLike.isOK(); })
            .withBackoffBetweenIterations(kBackoff)
            .on(**_taskExecutor, token);
    }

    void handleTransientError(const std::string& operationName, const Status& status);
    void handleUnrecoverableError(const std::string& operationName, const Status& status);

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
                     MovePrimaryDonorDependencies dependencies);

    SemiFuture<void> run(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                         const CancellationToken& stepdownToken) noexcept override;

    void interrupt(Status status) override;

    boost::optional<BSONObj> reportForCurrentOp(
        MongoProcessInterface::CurrentOpConnectionsMode connMode,
        MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept override;

    void checkIfOptionsConflict(const BSONObj& stateDoc) const override;

    const MovePrimaryCommonMetadata& getMetadata() const;

    SharedSemiFuture<void> getCompletionFuture() const;

private:
    MovePrimaryDonorStateEnum getCurrentState() const;
    MovePrimaryDonorMutableFields getMutableFields() const;
    MovePrimaryDonorDocument buildCurrentStateDocument() const;

    void initializeRun(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                       const CancellationToken& stepdownToken);
    ExecutorFuture<void> runDonorWorkflow();
    ExecutorFuture<void> transitionToState(MovePrimaryDonorStateEnum state);
    ExecutorFuture<void> doInitializing();
    ExecutorFuture<void> doCloning();
    void updateOnDiskState(OperationContext* opCtx,
                           const MovePrimaryDonorDocument& newStateDocument);
    void updateInMemoryState(const MovePrimaryDonorDocument& newStateDocument);

    template <typename Fn>
    auto runOnTaskExecutor(Fn&& fn) {
        return ExecutorFuture(**_taskExecutor).then(std::forward<Fn>(fn));
    }

    mutable Mutex _mutex = MONGO_MAKE_LATCH("MovePrimaryDonor::_mutex");
    ServiceContext* _serviceContext;
    MovePrimaryDonorService* const _donorService;
    const MovePrimaryCommonMetadata _metadata;

    MovePrimaryDonorMutableFields _mutableFields;
    std::unique_ptr<MovePrimaryMetrics> _metrics;

    std::shared_ptr<executor::ScopedTaskExecutor> _taskExecutor;
    boost::optional<MovePrimaryDonorCancelState> _cancelState;
    boost::optional<MovePrimaryDonorRetryHelper> _retry;

    std::unique_ptr<MovePrimaryDonorExternalState> _externalState;

    SharedPromise<void> _completionPromise;
};

}  // namespace mongo
