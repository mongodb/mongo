// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/repl/primary_only_service.h"
#include "mongo/db/s/migration_blocking_operation/multi_update_coordinator_external_state.h"
#include "mongo/db/s/migration_blocking_operation/multi_update_coordinator_gen.h"
#include "mongo/db/s/primary_only_service_helpers/retry_until_majority_commit.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {
using namespace std::literals::string_view_literals;

class MultiUpdateCoordinatorInstance;

class [[MONGO_MOD_PUBLIC]] MultiUpdateCoordinatorService : public repl::PrimaryOnlyService {
public:
    static constexpr std::string_view kServiceName = "MultiUpdateCoordinatorService"sv;

    friend MultiUpdateCoordinatorInstance;

    [[MONGO_MOD_NEEDS_REPLACEMENT]] static void abortAndWaitForAllInstances(OperationContext* opCtx,
                                                                            Status reason);

    MultiUpdateCoordinatorService(ServiceContext* serviceContext);

    MultiUpdateCoordinatorService(
        ServiceContext* serviceContext,
        std::unique_ptr<MultiUpdateCoordinatorExternalStateFactory> factory);

    [[MONGO_MOD_PRIVATE]] std::string_view getServiceName() const override;

    [[MONGO_MOD_PRIVATE]] NamespaceString getStateDocumentsNS() const override;

    [[MONGO_MOD_PRIVATE]] ThreadPoolLimits getThreadPoolLimits() const override;

    [[MONGO_MOD_PRIVATE]] void checkIfConflictsWithOtherInstances(
        OperationContext* opCtx,
        BSONObj initialState,
        const std::vector<const Instance*>& existingInstances) override;

    [[MONGO_MOD_PRIVATE]] std::shared_ptr<PrimaryOnlyService::Instance> constructInstance(
        BSONObj initialState) override;

private:
    std::unique_ptr<MultiUpdateCoordinatorExternalStateFactory> _externalStateFactory;
};

class [[MONGO_MOD_PRIVATE]] MultiUpdateCoordinatorInstance
    : public repl::PrimaryOnlyService::TypedInstance<MultiUpdateCoordinatorInstance> {
public:
    MultiUpdateCoordinatorInstance(const MultiUpdateCoordinatorService* service,
                                   MultiUpdateCoordinatorDocument initialDocument);

    SemiFuture<void> run(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                         const CancellationToken& stepdownToken) noexcept override;

    void abort(Status reason);
    void interrupt(Status status) override;

    boost::optional<BSONObj> reportForCurrentOp(
        MongoProcessInterface::CurrentOpConnectionsMode connMode,
        MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept override;

    void checkIfOptionsConflict(const BSONObj& stateDoc) const override;

    const MultiUpdateCoordinatorMetadata& getMetadata() const;

    SharedSemiFuture<BSONObj> getCompletionFuture() const;

private:
    const boost::optional<Status>& _getAbortReason() const;
    StatusWith<BSONObj> _getResult() const;

    MultiUpdateCoordinatorMutableFields _getMutableFields() const;
    MultiUpdateCoordinatorPhaseEnum _getCurrentPhase() const;
    MultiUpdateCoordinatorDocument _buildCurrentStateDocument() const;

    void _acquireSession();
    void _releaseSession();
    const LogicalSessionId& _getSessionId() const;
    bool _sessionIsCheckedOut() const;
    bool _sessionIsPersisted() const;
    bool _shouldReleaseSession() const;
    bool _shouldUnblockMigrations() const;
    bool _updatesPossiblyRunningFromPreviousTerm() const;
    bool _currentlySteppingDown() const;
    bool _updateWouldImplicitlyCreateCollection(OperationContext* opCtx) const;

    void _initializeRun(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                        const CancellationToken& stepdownToken);
    ExecutorFuture<void> _transitionToPhase(MultiUpdateCoordinatorPhaseEnum newPhase);

    void _updateInMemoryState(const MultiUpdateCoordinatorDocument& newStateDocument);
    void _updateOnDiskState(OperationContext* opCtx,
                            const MultiUpdateCoordinatorDocument& newStateDocument);

    ExecutorFuture<BSONObj> _runWorkflow();
    ExecutorFuture<void> _doAcquireSessionPhase();
    ExecutorFuture<void> _doBlockMigrationsPhase();
    ExecutorFuture<void> _doPerformUpdatePhase();
    ExecutorFuture<void> _stopBlockingMigrationsIfNeeded();

    Message getUpdateAsClusterCommand() const;
    ExecutorFuture<void> _sendUpdateRequest();
    ExecutorFuture<void> _waitForPendingUpdates();


    const MultiUpdateCoordinatorService* const _service;

    mutable std::mutex _mutex;
    const MultiUpdateCoordinatorMetadata _metadata;
    MultiUpdateCoordinatorMutableFields _mutableFields;
    const MultiUpdateCoordinatorPhaseEnum _beganInPhase;
    std::unique_ptr<MultiUpdateCoordinatorExternalState> _externalState;

    std::shared_ptr<executor::ScopedTaskExecutor> _taskExecutor;
    boost::optional<primary_only_service_helpers::CancelState> _cancelState;
    boost::optional<primary_only_service_helpers::RetryUntilMajorityCommit> _retry;

    SharedPromise<BSONObj> _completionPromise;
    boost::optional<BSONObj> _cmdResponse;
    boost::optional<Status> _abortReason;
};

}  // namespace mongo
