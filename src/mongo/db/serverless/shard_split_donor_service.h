/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/client/replica_set_monitor.h"
#include "mongo/client/sdam/sdam_datatypes.h"
#include "mongo/db/cancelable_operation_context.h"
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/serverless/shard_split_state_machine_gen.h"
#include "mongo/executor/cancelable_executor.h"

namespace mongo {

using TaskExecutorPtr = std::shared_ptr<executor::TaskExecutor>;
using ScopedTaskExecutorPtr = std::shared_ptr<executor::ScopedTaskExecutor>;

class ShardSplitDonorService final : public repl::PrimaryOnlyService {
public:
    static constexpr StringData kServiceName = "ShardSplitDonorService"_sd;

    explicit ShardSplitDonorService(ServiceContext* const serviceContext)
        : PrimaryOnlyService(serviceContext), _serviceContext(serviceContext) {}
    ~ShardSplitDonorService() = default;

    class DonorStateMachine;

    StringData getServiceName() const override {
        return kServiceName;
    }

    NamespaceString getStateDocumentsNS() const override {
        return NamespaceString::kShardSplitDonorsNamespace;
    }

    ThreadPool::Limits getThreadPoolLimits() const override;

    void abortAllSplits(OperationContext* opCtx);

protected:
    // Instance conflict check not yet implemented.
    void checkIfConflictsWithOtherInstances(
        OperationContext* opCtx,
        BSONObj initialState,
        const std::vector<const repl::PrimaryOnlyService::Instance*>& existingInstances) override;

    std::shared_ptr<PrimaryOnlyService::Instance> constructInstance(BSONObj initialState) override;

private:
    ExecutorFuture<void> _createStateDocumentTTLIndex(
        std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancellationToken& token);

    ServiceContext* const _serviceContext;
};

class ShardSplitDonorService::DonorStateMachine final
    : public repl::PrimaryOnlyService::TypedInstance<DonorStateMachine> {
public:
    struct DurableState {
        ShardSplitDonorStateEnum state;
        boost::optional<Status> abortReason;
    };

    DonorStateMachine(ServiceContext* serviceContext,
                      ShardSplitDonorService* serviceInstance,
                      const ShardSplitDonorDocument& initialState);

    ~DonorStateMachine() = default;

    /**
     * Try to abort this split operation. If the split operation is uninitialized, this will
     * durably record the operation as aborted.
     */
    void tryAbort();

    /**
     * Try to forget the shard split operation. If the operation is not in a final state, the
     * promise will be set but the garbage collection will be skipped.
     */
    void tryForget();

    void checkIfOptionsConflict(const BSONObj& stateDoc) const final;

    SemiFuture<void> run(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                         const CancellationToken& token) noexcept override;

    void interrupt(Status status) override;

    SharedSemiFuture<DurableState> decisionFuture() const {
        return _decisionPromise.getFuture();
    }

    SharedSemiFuture<void> completionFuture() const {
        return _completionPromise.getFuture();
    }

    SharedSemiFuture<void> garbageCollectableFuture() const {
        return _garbageCollectablePromise.getFuture();
    }

    UUID getId() const {
        return _migrationId;
    }

    /**
     * Report TenantMigrationDonorService Instances in currentOp().
     */
    boost::optional<BSONObj> reportForCurrentOp(
        MongoProcessInterface::CurrentOpConnectionsMode connMode,
        MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept override;

    /**
     * Returns true if the state doc was marked to expire (marked garbage collectable).
     */
    bool isGarbageCollectable() const {
        stdx::lock_guard<Latch> lg(_mutex);
        return !!_stateDoc.getExpireAt();
    }

    /**
     * Only used for testing. Allows settinga custom task executor for observing split acceptance.
     */
    static void setSplitAcceptanceTaskExecutor_forTest(TaskExecutorPtr taskExecutor) {
        _splitAcceptanceTaskExecutorForTest = taskExecutor;
    }

    ShardSplitDonorStateEnum getStateDocState() const {
        stdx::lock_guard<Latch> lg(_mutex);
        return _stateDoc.getState();
    }

    SharedSemiFuture<HostAndPort> getSplitAcceptanceFuture_forTest() const {
        return _splitAcceptancePromise.getFuture();
    }

private:
    // Tasks
    ExecutorFuture<void> _enterAbortIndexBuildsOrAbortedState(const ScopedTaskExecutorPtr& executor,
                                                              const CancellationToken& primaryToken,
                                                              const CancellationToken& abortToken);

    ExecutorFuture<void> _abortIndexBuildsAndEnterBlockingState(
        const ScopedTaskExecutorPtr& executor, const CancellationToken& abortToken);

    ExecutorFuture<void> _waitForRecipientToReachBlockTimestamp(
        const ScopedTaskExecutorPtr& executor, const CancellationToken& abortToken);

    ExecutorFuture<void> _applySplitConfigToDonor(const ScopedTaskExecutorPtr& executor,
                                                  const CancellationToken& abortToken);

    ExecutorFuture<void> _waitForSplitAcceptanceAndEnterCommittedState(
        const ScopedTaskExecutorPtr& executor,
        const CancellationToken& primaryToken,
        const CancellationToken& abortToken);

    ExecutorFuture<void> _waitForForgetCmdThenMarkGarbageCollectable(
        const ScopedTaskExecutorPtr& executor, const CancellationToken& primaryToken);

    ExecutorFuture<void> _waitForGarbageCollectionTimeoutThenDeleteStateDoc(
        const ScopedTaskExecutorPtr& executor, const CancellationToken& primaryToken);

    ExecutorFuture<void> _removeSplitConfigFromDonor(const ScopedTaskExecutorPtr& executor,
                                                     const CancellationToken& primaryToken);

    ExecutorFuture<DurableState> _handleErrorOrEnterAbortedState(
        Status status,
        const ScopedTaskExecutorPtr& executor,
        const CancellationToken& instanceAbortToken,
        const CancellationToken& abortToken);

    // Helpers
    ExecutorFuture<repl::OpTime> _updateStateDocument(const ScopedTaskExecutorPtr& executor,
                                                      const CancellationToken& token,
                                                      ShardSplitDonorStateEnum nextState);

    ExecutorFuture<void> _waitForMajorityWriteConcern(const ScopedTaskExecutorPtr& executor,
                                                      repl::OpTime opTime,
                                                      const CancellationToken& token);

    void _initiateTimeout(const ScopedTaskExecutorPtr& executor,
                          const CancellationToken& abortToken);
    ConnectionString _setupAcceptanceMonitoring(WithLock lock, const CancellationToken& abortToken);
    bool _hasInstalledSplitConfig(WithLock lock);

    /*
     * We need to call this method when we find out the replica set name is the same as the state
     * doc recipient set name and the current state doc state is blocking.
     */
    ExecutorFuture<void> _cleanRecipientStateDoc(const ScopedTaskExecutorPtr& executor,
                                                 const CancellationToken& token);

private:
    const NamespaceString _stateDocumentsNS = NamespaceString::kShardSplitDonorsNamespace;
    mutable Mutex _mutex = MONGO_MAKE_LATCH("ShardSplitDonorService::_mutex");

    const UUID _migrationId;
    ServiceContext* const _serviceContext;
    ShardSplitDonorService* const _shardSplitService;
    ShardSplitDonorDocument _stateDoc;

    // ThreadPool used by CancelableOperationContext.
    // CancelableOperationContext must have a thread that is always available to it to mark its
    // opCtx as killed when the cancelToken has been cancelled.
    const std::shared_ptr<ThreadPool> _markKilledExecutor;
    boost::optional<CancelableOperationContextFactory> _cancelableOpCtxFactory;

    bool _abortRequested = false;
    boost::optional<CancellationSource> _abortSource;
    boost::optional<Status> _abortReason;

    // A promise fulfilled when the shard split has committed or aborted.
    SharedPromise<DurableState> _decisionPromise;

    // A promise fulfilled when the shard split state document has been removed following the
    // completion of the operation.
    SharedPromise<void> _completionPromise;

    // A promise fulfilled when expireAt has been set following the end of the split.
    SharedPromise<void> _garbageCollectablePromise;

    // A promise fulfilled when all recipient nodes have accepted the split.
    SharedPromise<HostAndPort> _splitAcceptancePromise;

    // A promise fulfilled when tryForget is called.
    SharedPromise<void> _forgetShardSplitReceivedPromise;

    // A task executor used for the split acceptance future in tests
    static boost::optional<TaskExecutorPtr> _splitAcceptanceTaskExecutorForTest;
};

}  // namespace mongo
