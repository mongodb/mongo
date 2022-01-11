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
#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/serverless/shard_split_state_machine_gen.h"
#include "mongo/executor/cancelable_executor.h"

namespace mongo {

using ScopedTaskExecutorPtr = std::shared_ptr<executor::ScopedTaskExecutor>;

std::function<bool(const std::vector<sdam::ServerDescriptionPtr>&)>
makeRecipientAcceptSplitPredicate(std::string name, int expectedSize);

SemiFuture<void> getRecipientAcceptSplitFuture(ExecutorPtr executor,
                                               const CancellationToken& token,
                                               MongoURI recipientConnectionString);

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
        return NamespaceString::kTenantSplitDonorsNamespace;
    }

    ThreadPool::Limits getThreadPoolLimits() const override;

protected:
    // Instance conflict check not yet implemented.
    void checkIfConflictsWithOtherInstances(
        OperationContext* opCtx,
        BSONObj initialState,
        const std::vector<const repl::PrimaryOnlyService::Instance*>& existingInstances) override{};

    std::shared_ptr<PrimaryOnlyService::Instance> constructInstance(BSONObj initialState) override;

private:
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

    Status checkIfOptionsConflict(const ShardSplitDonorDocument& stateDoc) const;

    SemiFuture<void> run(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                         const CancellationToken& token) noexcept override;

    void interrupt(Status status) override;

    SharedSemiFuture<DurableState> completionFuture() const {
        return _completionPromise.getFuture();
    }

    SharedSemiFuture<void> replicaSetMonitorCreatedFuture() const {
        return _replicaSetMonitorCreatedPromise.getFuture();
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

private:
    // Tasks
    ExecutorFuture<void> _enterDataSyncState(const ScopedTaskExecutorPtr& executor,
                                             const CancellationToken& token);

    ExecutorFuture<void> _waitForRecipientToAcceptSplit(const ScopedTaskExecutorPtr& executor,
                                                        const CancellationToken& token);

    // Helpers
    ExecutorFuture<void> _writeInitialDocument(const ScopedTaskExecutorPtr& executor,
                                               const CancellationToken& token);

    ExecutorFuture<repl::OpTime> _updateStateDocument(const ScopedTaskExecutorPtr& executor,
                                                      const CancellationToken& token);

    ExecutorFuture<void> _waitForMajorityWriteConcern(const ScopedTaskExecutorPtr& executor,
                                                      repl::OpTime opTime,
                                                      const CancellationToken& token);

    void _createReplicaSetMonitor(const ExecutorPtr& executor, const CancellationToken& abortToken);

    ExecutorFuture<DurableState> _handleErrorOrEnterAbortedState(
        StatusWith<DurableState> durableState,
        const ScopedTaskExecutorPtr& executor,
        const CancellationToken& instanceAbortToken,
        const CancellationToken& abortToken);

private:
    const NamespaceString _stateDocumentsNS = NamespaceString::kTenantSplitDonorsNamespace;
    mutable Mutex _mutex = MONGO_MAKE_LATCH("ShardSplitDonorService::_mutex");

    const UUID _migrationId;
    ServiceContext* const _serviceContext;
    ShardSplitDonorService* const _shardSplitService;
    ShardSplitDonorDocument _stateDoc;

    bool _abortRequested = false;
    boost::optional<CancellationSource> _abortSource;
    boost::optional<Status> _abortReason;

    // A promise fulfilled when the replicaSetMonitor has been created;
    SharedPromise<void> _replicaSetMonitorCreatedPromise;

    // A promise fulfilled when the shard split operation has fully completed
    SharedPromise<DurableState> _completionPromise;

    // A promise fulfilled when all recipient nodes have accepted the split.
    SharedPromise<void> _recipientAcceptedSplit;
};

}  // namespace mongo
