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


#include "mongo/db/serverless/shard_split_donor_service.h"
#include "mongo/client/streamable_replica_set_monitor.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/db/serverless/shard_split_statistics.h"
#include "mongo/db/serverless/shard_split_utils.h"
#include "mongo/executor/cancelable_executor.h"
#include "mongo/executor/connection_pool.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/metadata/egress_metadata_hook_list.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_util.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTenantMigration


namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(abortShardSplitBeforeLeavingBlockingState);
MONGO_FAIL_POINT_DEFINE(pauseShardSplitBeforeBlockingState);
MONGO_FAIL_POINT_DEFINE(pauseShardSplitAfterBlocking);
MONGO_FAIL_POINT_DEFINE(pauseShardSplitAfterDecision);
MONGO_FAIL_POINT_DEFINE(skipShardSplitWaitForSplitAcceptance);
MONGO_FAIL_POINT_DEFINE(pauseShardSplitBeforeRecipientCleanup);
MONGO_FAIL_POINT_DEFINE(pauseShardSplitAfterMarkingStateGarbageCollectable);
MONGO_FAIL_POINT_DEFINE(pauseShardSplitBeforeSplitConfigRemoval);
MONGO_FAIL_POINT_DEFINE(skipShardSplitRecipientCleanup);
MONGO_FAIL_POINT_DEFINE(pauseShardSplitBeforeLeavingBlockingState);
MONGO_FAIL_POINT_DEFINE(pauseShardSplitAfterUpdatingToCommittedState);
MONGO_FAIL_POINT_DEFINE(pauseShardSplitBeforeSendingStepUpToRecipients);
MONGO_FAIL_POINT_DEFINE(pauseShardSplitAfterReceivingAbortCmd);

const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());

bool isAbortedDocumentPersistent(WithLock, ShardSplitDonorDocument& stateDoc) {
    return !!stateDoc.getAbortReason();
}

void checkForTokenInterrupt(const CancellationToken& token) {
    uassert(ErrorCodes::CallbackCanceled, "Donor service interrupted", !token.isCanceled());
}

void insertTenantAccessBlocker(WithLock lk,
                               OperationContext* opCtx,
                               ShardSplitDonorDocument donorStateDoc) {
    auto optionalTenants = donorStateDoc.getTenantIds();
    invariant(optionalTenants);
    auto recipientConnectionString = donorStateDoc.getRecipientConnectionString();
    invariant(recipientConnectionString);

    for (const auto& tenantId : optionalTenants.get()) {
        auto mtab = std::make_shared<TenantMigrationDonorAccessBlocker>(
            opCtx->getServiceContext(),
            donorStateDoc.getId(),
            tenantId.toString(),
            MigrationProtocolEnum::kMultitenantMigrations,
            recipientConnectionString->toString());

        TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext()).add(tenantId, mtab);

        // If the wuow fails, we need to remove the access blockers we just added. This ensures we
        // leave things in a valid state and are able to retry the operation.
        opCtx->recoveryUnit()->onRollback([opCtx, donorStateDoc, tenant = tenantId.toString()] {
            TenantMigrationAccessBlockerRegistry::get(opCtx->getServiceContext())
                .remove(tenant, TenantMigrationAccessBlocker::BlockerType::kDonor);
        });
    }
}

const std::string kTTLIndexName = "ShardSplitDonorTTLIndex";

}  // namespace

namespace detail {

SemiFuture<void> makeRecipientAcceptSplitFuture(
    std::shared_ptr<executor::TaskExecutor> taskExecutor,
    const CancellationToken& abortToken,
    const ConnectionString& recipientConnectionString,
    const UUID migrationId) {

    // build a vector of single server discovery monitors to listen for heartbeats
    auto eventsPublisher = std::make_shared<sdam::TopologyEventsPublisher>(taskExecutor);

    auto listener = std::make_shared<mongo::serverless::RecipientAcceptSplitListener>(
        recipientConnectionString);
    eventsPublisher->registerListener(listener);

    auto managerStats = std::make_shared<ReplicaSetMonitorManagerStats>();
    auto stats = std::make_shared<ReplicaSetMonitorStats>(managerStats);
    auto recipientNodes = recipientConnectionString.getServers();

    std::vector<SingleServerDiscoveryMonitorPtr> monitors;
    for (const auto& server : recipientNodes) {
        SdamConfiguration sdamConfiguration(std::vector<HostAndPort>{server});
        auto connectionString = ConnectionString::forStandalones(std::vector<HostAndPort>{server});

        monitors.push_back(
            std::make_shared<SingleServerDiscoveryMonitor>(MongoURI{connectionString},
                                                           server,
                                                           boost::none,
                                                           sdamConfiguration,
                                                           eventsPublisher,
                                                           taskExecutor,
                                                           stats));
        monitors.back()->init();
    }

    return future_util::withCancellation(listener->getFuture(), abortToken)
        .thenRunOn(taskExecutor)
        // Preserve lifetime of listener and monitor until the future is fulfilled and remove the
        // listener.
        .onCompletion(
            [monitors = std::move(monitors), listener, eventsPublisher, taskExecutor, migrationId](
                Status s) {
                eventsPublisher->close();

                for (auto& monitor : monitors) {
                    monitor->shutdown();
                }

                return s;
            })
        .semi();
}

}  // namespace detail

ThreadPool::Limits ShardSplitDonorService::getThreadPoolLimits() const {
    return ThreadPool::Limits();
}

void ShardSplitDonorService::checkIfConflictsWithOtherInstances(
    OperationContext* opCtx,
    BSONObj initialState,
    const std::vector<const repl::PrimaryOnlyService::Instance*>& existingInstances) {
    auto stateDoc = ShardSplitDonorDocument::parse(IDLParserContext("donorStateDoc"), initialState);

    for (auto& instance : existingInstances) {
        auto existingTypedInstance =
            checked_cast<const ShardSplitDonorService::DonorStateMachine*>(instance);
        bool isGarbageCollectable = existingTypedInstance->isGarbageCollectable();
        bool existingIsAborted =
            existingTypedInstance->getStateDocState() == ShardSplitDonorStateEnum::kAborted &&
            isGarbageCollectable;

        uassert(ErrorCodes::ConflictingOperationInProgress,
                str::stream() << "Can't start a concurent shard split operation, currently running"
                              << " migrationId: " << existingTypedInstance->getId(),
                existingIsAborted);
    }
}

std::shared_ptr<repl::PrimaryOnlyService::Instance> ShardSplitDonorService::constructInstance(
    BSONObj initialState) {
    return std::make_shared<DonorStateMachine>(
        _serviceContext,
        this,
        ShardSplitDonorDocument::parse(IDLParserContext("donorStateDoc"), initialState));
}

void ShardSplitDonorService::abortAllSplits(OperationContext* opCtx) {
    LOGV2(8423361, "Aborting all active shard split operations.");
    auto instances = getAllInstances(opCtx);
    for (auto& instance : instances) {
        auto typedInstance =
            checked_pointer_cast<ShardSplitDonorService::DonorStateMachine>(instance);
        typedInstance->tryAbort();
    }
}

ExecutorFuture<void> ShardSplitDonorService::_createStateDocumentTTLIndex(
    std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancellationToken& token) {
    return AsyncTry([this] {
               auto nss = getStateDocumentsNS();

               AllowOpCtxWhenServiceRebuildingBlock allowOpCtxBlock(Client::getCurrent());
               auto opCtxHolder = cc().makeOperationContext();
               auto opCtx = opCtxHolder.get();
               DBDirectClient client(opCtx);

               BSONObj result;
               client.runCommand(
                   nss.db().toString(),
                   BSON("createIndexes"
                        << nss.coll().toString() << "indexes"
                        << BSON_ARRAY(BSON("key" << BSON("expireAt" << 1) << "name" << kTTLIndexName
                                                 << "expireAfterSeconds" << 0))),
                   result);
               uassertStatusOK(getStatusFromCommandResult(result));
           })
        .until([](Status status) { return status.isOK(); })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, token);
}

ExecutorFuture<void> ShardSplitDonorService::_rebuildService(
    std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancellationToken& token) {
    if (!repl::feature_flags::gShardSplit.isEnabled(serverGlobalParams.featureCompatibility)) {
        return ExecutorFuture(**executor);
    }
    return _createStateDocumentTTLIndex(executor, token);
}

boost::optional<TaskExecutorPtr>
    ShardSplitDonorService::DonorStateMachine::_splitAcceptanceTaskExecutorForTest;
ShardSplitDonorService::DonorStateMachine::DonorStateMachine(
    ServiceContext* serviceContext,
    ShardSplitDonorService* splitService,
    const ShardSplitDonorDocument& initialState)
    : repl::PrimaryOnlyService::TypedInstance<DonorStateMachine>(),
      _migrationId(initialState.getId()),
      _serviceContext(serviceContext),
      _shardSplitService(splitService),
      _stateDoc(initialState),
      _markKilledExecutor(std::make_shared<ThreadPool>([] {
          ThreadPool::Options options;
          options.poolName = "ShardSplitCancelableOpCtxPool";
          options.minThreads = 1;
          options.maxThreads = 1;
          return options;
      }())) {}

void ShardSplitDonorService::DonorStateMachine::tryAbort() {
    LOGV2(6086502, "Received 'abortShardSplit' command.", "id"_attr = _migrationId);
    {
        stdx::lock_guard<Latch> lg(_mutex);
        _abortRequested = true;
        if (_abortSource) {
            _abortSource->cancel();
        }
    }
    pauseShardSplitAfterReceivingAbortCmd.pauseWhileSet();
}

void ShardSplitDonorService::DonorStateMachine::tryForget() {
    LOGV2(6236601, "Received 'forgetShardSplit' command.", "id"_attr = _migrationId);
    stdx::lock_guard<Latch> lg(_mutex);
    if (_forgetShardSplitReceivedPromise.getFuture().isReady()) {
        return;
    }

    _forgetShardSplitReceivedPromise.emplaceValue();
}

void ShardSplitDonorService::DonorStateMachine::checkIfOptionsConflict(
    const BSONObj& stateDocBson) const {
    auto stateDoc = ShardSplitDonorDocument::parse(IDLParserContext("donorStateDoc"), stateDocBson);

    stdx::lock_guard<Latch> lg(_mutex);
    invariant(stateDoc.getId() == _stateDoc.getId());

    if (_stateDoc.getTenantIds() != stateDoc.getTenantIds() ||
        _stateDoc.getRecipientTagName() != stateDoc.getRecipientTagName() ||
        _stateDoc.getRecipientSetName() != stateDoc.getRecipientSetName()) {
        uasserted(ErrorCodes::ConflictingOperationInProgress,
                  str::stream() << "Found active migration for migrationId \""
                                << _stateDoc.getId().toBSON() << "\" with different options "
                                << _stateDoc.toBSON());
    }
}

SemiFuture<void> ShardSplitDonorService::DonorStateMachine::run(
    ScopedTaskExecutorPtr executor, const CancellationToken& primaryToken) noexcept {
    auto abortToken = [&]() {
        stdx::lock_guard<Latch> lg(_mutex);
        _abortSource = CancellationSource(primaryToken);
        if (_abortRequested || _stateDoc.getState() == ShardSplitDonorStateEnum::kAborted) {
            _abortSource->cancel();
        }

        // We must abort the migration if we try to start or resume while upgrading or downgrading.
        // (Generic FCV reference): This FCV check should exist across LTS binary versions.
        if (serverGlobalParams.featureCompatibility.isUpgradingOrDowngrading()) {
            LOGV2(8423360, "Aborting shard split since donor is upgrading or downgrading.");
            _abortSource->cancel();
        }

        return _abortSource->token();
    }();

    _markKilledExecutor->startup();
    _cancelableOpCtxFactory.emplace(primaryToken, _markKilledExecutor);

    auto criticalSectionTimer = std::make_shared<Timer>();
    auto criticalSectionWithoutCatchupTimer = std::make_shared<Timer>();

    const bool shouldRemoveStateDocumentOnRecipient = [&]() {
        auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
        stdx::lock_guard<Latch> lg(_mutex);
        return serverless::shouldRemoveStateDocumentOnRecipient(opCtx.get(), _stateDoc);
    }();

    _decisionPromise.setWith([&] {
        if (shouldRemoveStateDocumentOnRecipient) {
            pauseShardSplitBeforeRecipientCleanup.pauseWhileSet();

            return ExecutorFuture(**executor)
                .then([this, executor, primaryToken, anchor = shared_from_this()] {
                    if (MONGO_unlikely(skipShardSplitRecipientCleanup.shouldFail())) {
                        return ExecutorFuture(**executor);
                    }

                    return _cleanRecipientStateDoc(executor, primaryToken);
                })
                .then([this, executor, migrationId = _migrationId]() {
                    return DurableState{ShardSplitDonorStateEnum::kCommitted};
                })
                .unsafeToInlineFuture();
        }

        LOGV2(6086506,
              "Starting shard split.",
              "id"_attr = _migrationId,
              "timeout"_attr = repl::shardSplitTimeoutMS.load());

        auto isConfigValidWithStatus = [&]() {
            stdx::lock_guard<Latch> lg(_mutex);
            auto replCoord = repl::ReplicationCoordinator::get(cc().getServiceContext());
            invariant(replCoord);
            return serverless::validateRecipientNodesForShardSplit(_stateDoc,
                                                                   replCoord->getConfig());
        }();

        if (!isConfigValidWithStatus.isOK()) {
            stdx::lock_guard<Latch> lg(_mutex);

            LOGV2_ERROR(6395900,
                        "Failed to validate recipient nodes for shard split.",
                        "id"_attr = _migrationId,
                        "status"_attr = isConfigValidWithStatus);

            _abortReason = isConfigValidWithStatus;
        }

        _initiateTimeout(executor, abortToken);
        return ExecutorFuture(**executor)
            .then([this, executor, primaryToken, abortToken] {
                // Note we do not use the abort split token here because the abortShardSplit
                // command waits for a decision to be persisted which will not happen if
                // inserting the initial state document fails.
                return _enterAbortIndexBuildsOrAbortedState(executor, primaryToken, abortToken);
            })
            .then([this, executor, abortToken] {
                // Start tracking the abortToken for killing operation contexts
                _cancelableOpCtxFactory.emplace(abortToken, _markKilledExecutor);
                return _abortIndexBuildsAndEnterBlockingState(executor, abortToken);
            })
            .then([this, executor, abortToken, criticalSectionTimer] {
                criticalSectionTimer->reset();

                auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                pauseShardSplitAfterBlocking.pauseWhileSet(opCtx.get());

                return _waitForRecipientToReachBlockTimestamp(executor, abortToken);
            })
            .then([this, executor, abortToken, criticalSectionWithoutCatchupTimer] {
                criticalSectionWithoutCatchupTimer->reset();
                return _applySplitConfigToDonor(executor, abortToken);
            })
            .then([this, executor, abortToken] {
                return _waitForRecipientToAcceptSplit(executor, abortToken);
            })
            .then([this, executor, primaryToken] {
                // only cancel operations on stepdown from here out
                _cancelableOpCtxFactory.emplace(primaryToken, _markKilledExecutor);
                return _triggerElectionAndEnterCommitedState(executor, primaryToken);
            })
            // anchor ensures the instance will still exists even if the primary stepped down
            .onCompletion([this,
                           executor,
                           primaryToken,
                           abortToken,
                           criticalSectionTimer,
                           criticalSectionWithoutCatchupTimer,
                           anchor = shared_from_this()](Status status) {
                // only cancel operations on stepdown from here out
                _cancelableOpCtxFactory.emplace(primaryToken, _markKilledExecutor);

                LOGV2(6236700,
                      "Shard split decision reached",
                      "id"_attr = _migrationId,
                      "state"_attr = ShardSplitDonorState_serializer(_stateDoc.getState()),
                      "status"_attr = status);

                {
                    stdx::lock_guard<Latch> lg(_mutex);
                    if (!_stateDoc.getExpireAt()) {
                        if (_abortReason) {
                            ShardSplitStatistics::get(_serviceContext)->incrementTotalAborted();
                        } else {
                            ShardSplitStatistics::get(_serviceContext)
                                ->incrementTotalCommitted(
                                    Milliseconds{criticalSectionTimer->millis()},
                                    Milliseconds{criticalSectionWithoutCatchupTimer->millis()});
                        }
                    }
                }

                if (!status.isOK()) {
                    return _handleErrorOrEnterAbortedState(
                        status, executor, primaryToken, abortToken);
                }

                return ExecutorFuture(**executor, DurableState{_stateDoc.getState(), _abortReason});
            })
            .unsafeToInlineFuture();
    });

    _completionPromise.setWith([&] {
        if (shouldRemoveStateDocumentOnRecipient) {
            return ExecutorFuture(**executor)
                .then([&] { return _decisionPromise.getFuture().semi().ignoreValue(); })
                .onCompletion(
                    [this, executor, anchor = shared_from_this(), primaryToken](Status status) {
                        if (!status.isOK()) {
                            LOGV2_ERROR(6753100,
                                        "Failed to cleanup the state document on recipient nodes",
                                        "id"_attr = _migrationId,
                                        "abortReason"_attr = _abortReason,
                                        "status"_attr = status);
                        } else {
                            LOGV2(6753101,
                                  "Successfully cleaned up the state document on recipient nodes.",
                                  "id"_attr = _migrationId,
                                  "abortReason"_attr = _abortReason,
                                  "status"_attr = status);
                        }
                    })
                .unsafeToInlineFuture();
        }

        return ExecutorFuture(**executor)
            .then([&] { return _decisionPromise.getFuture().semi().ignoreValue(); })
            .onCompletion([this, executor, primaryToken](Status status) {
                // Always remove the split config, whether the operation was aborted or
                // committed.
                auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                pauseShardSplitBeforeSplitConfigRemoval.pauseWhileSetAndNotCanceled(opCtx.get(),
                                                                                    primaryToken);
                return _removeSplitConfigFromDonor(executor, primaryToken).then([status]() {
                    return status;
                });
            })
            .then([this, executor, primaryToken] {
                auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                pauseShardSplitAfterDecision.pauseWhileSet(opCtx.get());

                return _waitForForgetCmdThenMarkGarbageCollectable(executor, primaryToken);
            })
            .onCompletion([this, primaryToken, anchor = shared_from_this()](Status status) {
                stdx::lock_guard<Latch> lg(_mutex);
                // Propagate any errors from the donor stepping down.
                if (primaryToken.isCanceled() ||
                    _stateDoc.getState() < ShardSplitDonorStateEnum::kCommitted) {
                    return status;
                }

                LOGV2(8423356,
                      "Shard split completed.",
                      "id"_attr = _stateDoc.getId(),
                      "status"_attr = status,
                      "abortReason"_attr = _abortReason);

                return Status::OK();
            })
            .unsafeToInlineFuture();
    });

    return _completionPromise.getFuture().semi();
}

void ShardSplitDonorService::DonorStateMachine::interrupt(Status status) {}

boost::optional<BSONObj> ShardSplitDonorService::DonorStateMachine::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode connMode,
    MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept {

    stdx::lock_guard<Latch> lg(_mutex);
    BSONObjBuilder bob;
    bob.append("desc", "shard split operation");
    _migrationId.appendToBuilder(&bob, "instanceID"_sd);
    bob.append("reachedDecision", _decisionPromise.getFuture().isReady());
    if (_stateDoc.getExpireAt()) {
        bob.append("expireAt", *_stateDoc.getExpireAt());
    }
    const auto& tenantIds = _stateDoc.getTenantIds();
    if (tenantIds) {
        bob.append("tenantIds", *tenantIds);
    }
    if (_stateDoc.getBlockTimestamp()) {
        bob.append("blockTimestamp", *_stateDoc.getBlockTimestamp());
    }
    if (_stateDoc.getCommitOrAbortOpTime()) {
        _stateDoc.getCommitOrAbortOpTime()->append(&bob, "commitOrAbortOpTime");
    }
    if (_stateDoc.getAbortReason()) {
        bob.append("abortReason", *_stateDoc.getAbortReason());
    }
    if (_stateDoc.getRecipientConnectionString()) {
        bob.append("recipientConnectionString",
                   _stateDoc.getRecipientConnectionString()->toString());
    }
    if (_stateDoc.getRecipientSetName()) {
        bob.append("recipientSetName", *_stateDoc.getRecipientSetName());
    }
    if (_stateDoc.getRecipientTagName()) {
        bob.append("recipientTagName", *_stateDoc.getRecipientTagName());
    }

    return bob.obj();
}

bool ShardSplitDonorService::DonorStateMachine::_hasInstalledSplitConfig(WithLock lock) {
    auto replCoord = repl::ReplicationCoordinator::get(cc().getServiceContext());
    auto config = replCoord->getConfig();

    invariant(_stateDoc.getRecipientSetName());
    return config.isSplitConfig() &&
        config.getRecipientConfig()->getReplSetName() == *_stateDoc.getRecipientSetName();
}

ConnectionString ShardSplitDonorService::DonorStateMachine::_setupAcceptanceMonitoring(
    WithLock lock, const CancellationToken& abortToken) {
    auto recipientConnectionString = [stateDoc = _stateDoc]() {
        if (stateDoc.getRecipientConnectionString()) {
            return *stateDoc.getRecipientConnectionString();
        }

        auto recipientTagName = stateDoc.getRecipientTagName();
        invariant(recipientTagName);
        auto recipientSetName = stateDoc.getRecipientSetName();
        invariant(recipientSetName);
        auto config = repl::ReplicationCoordinator::get(cc().getServiceContext())->getConfig();
        return serverless::makeRecipientConnectionString(
            config, *recipientTagName, *recipientSetName);
    }();

    // Always start the replica set monitor if we haven't reached a decision yet
    _splitAcceptancePromise.setWith([&]() -> Future<void> {
        if (_stateDoc.getState() > ShardSplitDonorStateEnum::kBlocking ||
            MONGO_unlikely(skipShardSplitWaitForSplitAcceptance.shouldFail())) {
            return SemiFuture<void>::makeReady().unsafeToInlineFuture();
        }

        // Optionally select a task executor for unit testing
        auto executor = _splitAcceptanceTaskExecutorForTest
            ? *_splitAcceptanceTaskExecutorForTest
            : _shardSplitService->getInstanceCleanupExecutor();

        LOGV2(6142508,
              "Monitoring recipient nodes for split acceptance.",
              "id"_attr = _migrationId,
              "recipientConnectionString"_attr = recipientConnectionString);

        return detail::makeRecipientAcceptSplitFuture(
                   executor, abortToken, recipientConnectionString, _migrationId)
            .unsafeToInlineFuture();
    });

    return recipientConnectionString;
}

ExecutorFuture<void>
ShardSplitDonorService::DonorStateMachine::_enterAbortIndexBuildsOrAbortedState(
    const ScopedTaskExecutorPtr& executor,
    const CancellationToken& primaryToken,
    const CancellationToken& abortToken) {
    ShardSplitDonorStateEnum nextState;
    {
        stdx::lock_guard<Latch> lg(_mutex);
        if (_stateDoc.getState() == ShardSplitDonorStateEnum::kAborted || _abortReason) {
            if (isAbortedDocumentPersistent(lg, _stateDoc)) {
                // Node has step up and created an instance using a document in abort state. No
                // need to write the document as it already exists.
                _abortReason = mongo::resharding::getStatusFromAbortReason(_stateDoc);

                return ExecutorFuture(**executor);
            }

            if (!_abortReason) {
                _abortReason =
                    Status(ErrorCodes::TenantMigrationAborted, "Aborted due to 'abortShardSplit'.");
            }
            BSONObjBuilder bob;
            _abortReason->serializeErrorToBSON(&bob);
            _stateDoc.setAbortReason(bob.obj());
            _stateDoc.setExpireAt(_serviceContext->getFastClockSource()->now() +
                                  Milliseconds{repl::shardSplitGarbageCollectionDelayMS.load()});
            nextState = ShardSplitDonorStateEnum::kAborted;

            LOGV2(6670500, "Entering 'aborted' state.", "id"_attr = _stateDoc.getId());
        } else {
            // Always set up acceptance monitoring.
            auto recipientConnectionString = _setupAcceptanceMonitoring(lg, abortToken);

            if (_stateDoc.getState() > ShardSplitDonorStateEnum::kUninitialized) {
                // Node has stepped up and resumed a shard split. No need to write the document as
                // it already exists.
                return ExecutorFuture(**executor);
            }

            _stateDoc.setRecipientConnectionString(recipientConnectionString);
            nextState = ShardSplitDonorStateEnum::kAbortingIndexBuilds;

            LOGV2(
                6670501, "Entering 'aborting index builds' state.", "id"_attr = _stateDoc.getId());
        }
    }

    return _updateStateDocument(executor, primaryToken, nextState)
        .then([this, executor, primaryToken](repl::OpTime opTime) {
            return _waitForMajorityWriteConcern(executor, std::move(opTime), primaryToken);
        })
        .then([this, executor, nextState]() {
            uassert(ErrorCodes::TenantMigrationAborted,
                    "Shard split operation aborted.",
                    nextState != ShardSplitDonorStateEnum::kAborted);
        });
}

ExecutorFuture<void>
ShardSplitDonorService::DonorStateMachine::_abortIndexBuildsAndEnterBlockingState(
    const ScopedTaskExecutorPtr& executor, const CancellationToken& abortToken) {
    checkForTokenInterrupt(abortToken);

    boost::optional<std::vector<StringData>> tenantIds;
    {
        stdx::lock_guard<Latch> lg(_mutex);
        if (_stateDoc.getState() > ShardSplitDonorStateEnum::kAbortingIndexBuilds) {
            return ExecutorFuture(**executor);
        }

        tenantIds = _stateDoc.getTenantIds();
        invariant(tenantIds);
    }

    LOGV2(6436100, "Aborting index builds for shard split.", "id"_attr = _migrationId);

    // Abort any in-progress index builds. No new index builds can start while we are doing this
    // because the mtab prevents it.
    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
    auto* indexBuildsCoordinator = IndexBuildsCoordinator::get(opCtx.get());
    for (const auto& tenantId : *tenantIds) {
        indexBuildsCoordinator->abortTenantIndexBuilds(
            opCtx.get(), MigrationProtocolEnum::kMultitenantMigrations, tenantId, "shard split");
    }

    if (MONGO_unlikely(pauseShardSplitBeforeBlockingState.shouldFail())) {
        pauseShardSplitBeforeBlockingState.pauseWhileSet();
    }

    {
        stdx::lock_guard<Latch> lg(_mutex);
        LOGV2(8423358, "Entering 'blocking' state.", "id"_attr = _stateDoc.getId());
    }

    return _updateStateDocument(executor, abortToken, ShardSplitDonorStateEnum::kBlocking)
        .then([this, self = shared_from_this(), executor, abortToken](repl::OpTime opTime) {
            return _waitForMajorityWriteConcern(executor, std::move(opTime), abortToken);
        });
}

ExecutorFuture<void>
ShardSplitDonorService::DonorStateMachine::_waitForRecipientToReachBlockTimestamp(
    const ScopedTaskExecutorPtr& executor, const CancellationToken& abortToken) {
    checkForTokenInterrupt(abortToken);

    stdx::lock_guard<Latch> lg(_mutex);
    if (_stateDoc.getState() > ShardSplitDonorStateEnum::kBlocking ||
        _hasInstalledSplitConfig(lg)) {
        return ExecutorFuture(**executor);
    }

    auto replCoord = repl::ReplicationCoordinator::get(cc().getServiceContext());

    invariant(_stateDoc.getBlockTimestamp());
    auto blockTimestamp = *_stateDoc.getBlockTimestamp();
    repl::OpTime blockOpTime = repl::OpTime(blockTimestamp, replCoord->getConfigTerm());

    invariant(_stateDoc.getRecipientTagName());
    auto recipientTagName = *_stateDoc.getRecipientTagName();
    auto recipientNodes = serverless::getRecipientMembers(replCoord->getConfig(), recipientTagName);

    WriteConcernOptions writeConcern;
    writeConcern.w = WTags{{recipientTagName.toString(), recipientNodes.size()}};

    LOGV2(
        6177201, "Waiting for recipient nodes to reach block timestamp.", "id"_attr = _migrationId);

    return ExecutorFuture(**executor).then([this, blockOpTime, writeConcern]() {
        auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
        auto replCoord = repl::ReplicationCoordinator::get(cc().getServiceContext());
        uassertStatusOK(replCoord->awaitReplication(opCtx.get(), blockOpTime, writeConcern).status);
    });
}

ExecutorFuture<void> ShardSplitDonorService::DonorStateMachine::_applySplitConfigToDonor(
    const ScopedTaskExecutorPtr& executor, const CancellationToken& abortToken) {
    checkForTokenInterrupt(abortToken);

    {
        stdx::lock_guard<Latch> lg(_mutex);
        if (_stateDoc.getState() >= ShardSplitDonorStateEnum::kCommitted ||
            _hasInstalledSplitConfig(lg)) {
            return ExecutorFuture(**executor);
        }
    }

    auto splitConfig = [&]() {
        stdx::lock_guard<Latch> lg(_mutex);
        auto setName = _stateDoc.getRecipientSetName();
        invariant(setName);
        auto tagName = _stateDoc.getRecipientTagName();
        invariant(tagName);

        auto replCoord = repl::ReplicationCoordinator::get(cc().getServiceContext());
        invariant(replCoord);

        return serverless::makeSplitConfig(
            replCoord->getConfig(), setName->toString(), tagName->toString());
    }();

    LOGV2(6309100,
          "Applying the split config.",
          "id"_attr = _migrationId,
          "config"_attr = splitConfig);

    return AsyncTry([this, splitConfig] {
               auto opCtxHolder = _cancelableOpCtxFactory->makeOperationContext(&cc());
               DBDirectClient client(opCtxHolder.get());
               BSONObj result;
               const bool returnValue =
                   client.runCommand(NamespaceString::kAdminDb.toString(),
                                     BSON("replSetReconfig" << splitConfig.toBSON()),
                                     result);
               uassert(ErrorCodes::BadValue,
                       "Invalid return value for 'replSetReconfig' command.",
                       returnValue);
               uassertStatusOK(getStatusFromCommandResult(result));
           })
        .until([](Status status) { return status.isOK(); })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, abortToken);
}

ExecutorFuture<void> sendStepUpToRecipient(const HostAndPort recipient,
                                           TaskExecutorPtr executor,
                                           const CancellationToken& token) {
    return AsyncTry([executor, recipient, token] {
               executor::RemoteCommandRequest request(
                   recipient, "admin", BSON("replSetStepUp" << 1 << "skipDryRun" << true), nullptr);
               pauseShardSplitBeforeSendingStepUpToRecipients.pauseWhileSet();
               return executor->scheduleRemoteCommand(request, token)
                   .then([](const auto& response) {
                       return getStatusFromCommandResult(response.data);
                   });
           })
        .until([](Status status) {
            return status.isOK() ||
                (!ErrorCodes::isRetriableError(status) &&
                 !ErrorCodes::isNetworkTimeoutError(status));
        })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(executor, token);
}

ExecutorFuture<void> ShardSplitDonorService::DonorStateMachine::_waitForRecipientToAcceptSplit(
    const ScopedTaskExecutorPtr& executor, const CancellationToken& abortToken) {

    checkForTokenInterrupt(abortToken);
    {
        stdx::lock_guard<Latch> lg(_mutex);
        if (_stateDoc.getState() > ShardSplitDonorStateEnum::kBlocking) {
            return ExecutorFuture(**executor);
        }
    }

    LOGV2(6142501, "Waiting for recipient to accept the split.", "id"_attr = _migrationId);

    return ExecutorFuture(**executor).then([&]() { return _splitAcceptancePromise.getFuture(); });
}

ExecutorFuture<void>
ShardSplitDonorService::DonorStateMachine::_triggerElectionAndEnterCommitedState(
    const ScopedTaskExecutorPtr& executor, const CancellationToken& primaryToken) {
    checkForTokenInterrupt(primaryToken);

    std::vector<HostAndPort> recipients;
    {
        stdx::lock_guard<Latch> lg(_mutex);
        if (_stateDoc.getState() > ShardSplitDonorStateEnum::kBlocking) {
            return ExecutorFuture(**executor);
        }

        recipients = _stateDoc.getRecipientConnectionString()->getServers();
    }

    invariant(!recipients.empty());

    auto rng = std::default_random_engine{};
    std::shuffle(std::begin(recipients), std::end(recipients), rng);

    auto remoteCommandExecutor =
        _splitAcceptanceTaskExecutorForTest ? *_splitAcceptanceTaskExecutorForTest : **executor;

    return ExecutorFuture(**executor)
        .then([this] {
            auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
            if (MONGO_unlikely(pauseShardSplitBeforeLeavingBlockingState.shouldFail())) {
                pauseShardSplitBeforeLeavingBlockingState.execute([&](const BSONObj& data) {
                    if (!data.hasField("blockTimeMS")) {
                        pauseShardSplitBeforeLeavingBlockingState.pauseWhileSet(opCtx.get());
                    } else {
                        const auto blockTime = Milliseconds{data.getIntField("blockTimeMS")};
                        LOGV2(8423359,
                              "Keeping shard split in blocking state.",
                              "blockTime"_attr = blockTime);
                        opCtx->sleepFor(blockTime);
                    }
                });
            }

            if (MONGO_unlikely(abortShardSplitBeforeLeavingBlockingState.shouldFail())) {
                uasserted(ErrorCodes::InternalError, "simulate a shard split error");
            }
        })
        .then([this, recipients, primaryToken, remoteCommandExecutor] {
            LOGV2(6493901,
                  "Triggering an election after recipient has accepted the split.",
                  "id"_attr = _migrationId);

            // replSetStepUp on a random node will succeed as long as it's not the most out-of-date
            // node (in that case at least another node will vote for it and the election will
            // succeed). Selecting a random node has a 2/3 chance to succeed for replSetStepUp. If
            // the first command fail, we know this node is the most out-of-date. Therefore we
            // select the next node and we know the first node selected will vote for the second.
            return sendStepUpToRecipient(recipients[0], remoteCommandExecutor, primaryToken)
                .onCompletion(
                    [this, recipients, remoteCommandExecutor, primaryToken](Status status) {
                        if (status.isOK()) {
                            return ExecutorFuture<void>(remoteCommandExecutor, status);
                        }

                        return sendStepUpToRecipient(
                            recipients[1], remoteCommandExecutor, primaryToken);
                    })
                .onCompletion([this](Status replSetStepUpStatus) {
                    if (!replSetStepUpStatus.isOK()) {
                        LOGV2(6493904,
                              "Failed to trigger an election on the recipient replica set.",
                              "status"_attr = replSetStepUpStatus);
                    }

                    // Even if replSetStepUp failed, the recipient nodes have joined the
                    // recipient set. Therefore they will eventually elect a primary and the
                    // split will complete successfully, although slower than if the election
                    // succeeded.
                    return Status::OK();
                });
        })
        .thenRunOn(**executor)
        .then([this, executor, primaryToken]() {
            LOGV2(6142503, "Entering 'committed' state.", "id"_attr = _stateDoc.getId());
            auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
            pauseShardSplitAfterUpdatingToCommittedState.pauseWhileSet(opCtx.get());

            return _updateStateDocument(
                       executor, primaryToken, ShardSplitDonorStateEnum::kCommitted)
                .then([this, executor, primaryToken](repl::OpTime opTime) {
                    return _waitForMajorityWriteConcern(executor, std::move(opTime), primaryToken);
                });
        });
}

ExecutorFuture<repl::OpTime> ShardSplitDonorService::DonorStateMachine::_updateStateDocument(
    const ScopedTaskExecutorPtr& executor,
    const CancellationToken& token,
    ShardSplitDonorStateEnum nextState) {
    auto [tenantIds, isInsert, originalStateDocBson] = [&]() {
        stdx::lock_guard<Latch> lg(_mutex);
        auto currentState = _stateDoc.getState();
        auto isInsert = currentState == ShardSplitDonorStateEnum::kUninitialized ||
            currentState == ShardSplitDonorStateEnum::kAborted;
        return std::make_tuple(_stateDoc.getTenantIds(), isInsert, _stateDoc.toBSON());
    }();

    return AsyncTry([this,
                     tenantIds = std::move(tenantIds),
                     isInsert = isInsert,
                     originalStateDocBson = originalStateDocBson,
                     uuid = _migrationId,
                     nextState] {
               auto opCtxHolder = _cancelableOpCtxFactory->makeOperationContext(&cc());
               auto opCtx = opCtxHolder.get();

               AutoGetCollection collection(opCtx, _stateDocumentsNS, MODE_IX);

               if (!isInsert) {
                   uassert(ErrorCodes::NamespaceNotFound,
                           str::stream() << _stateDocumentsNS.ns() << " does not exist",
                           collection);
               }

               writeConflictRetry(
                   opCtx, "ShardSplitDonorUpdateStateDoc", _stateDocumentsNS.ns(), [&]() {
                       WriteUnitOfWork wuow(opCtx);

                       if (nextState == ShardSplitDonorStateEnum::kBlocking) {
                           // Start blocking writes before getting an oplog slot to guarantee no
                           // writes to the tenant's data can commit with a timestamp after the
                           // block timestamp.
                           for (const auto& tenantId : *tenantIds) {
                               auto mtab = tenant_migration_access_blocker::
                                   getTenantMigrationDonorAccessBlocker(_serviceContext, tenantId);
                               invariant(mtab);
                               mtab->startBlockingWrites();

                               opCtx->recoveryUnit()->onRollback(
                                   [mtab] { mtab->rollBackStartBlocking(); });
                           }
                       }

                       // Reserve an opTime for the write.
                       auto oplogSlot = LocalOplogInfo::get(opCtx)->getNextOpTimes(opCtx, 1U)[0];
                       auto updatedStateDocBson = [&]() {
                           stdx::lock_guard<Latch> lg(_mutex);
                           _stateDoc.setState(nextState);
                           switch (nextState) {
                               case ShardSplitDonorStateEnum::kUninitialized:
                               case ShardSplitDonorStateEnum::kAbortingIndexBuilds:
                                   break;
                               case ShardSplitDonorStateEnum::kBlocking:
                                   _stateDoc.setBlockTimestamp(oplogSlot.getTimestamp());
                                   break;
                               case ShardSplitDonorStateEnum::kCommitted:
                                   _stateDoc.setCommitOrAbortOpTime(oplogSlot);
                                   break;
                               case ShardSplitDonorStateEnum::kAborted: {
                                   _stateDoc.setCommitOrAbortOpTime(oplogSlot);

                                   invariant(_abortReason);
                                   BSONObjBuilder bob;
                                   _abortReason.get().serializeErrorToBSON(&bob);
                                   _stateDoc.setAbortReason(bob.obj());
                                   break;
                               }
                               default:
                                   MONGO_UNREACHABLE;
                           }
                           if (isInsert) {
                               return BSON("$setOnInsert" << _stateDoc.toBSON());
                           }

                           return _stateDoc.toBSON();
                       }();

                       auto updateOpTime = [&]() {
                           if (isInsert) {
                               const auto filter =
                                   BSON(ShardSplitDonorDocument::kIdFieldName << uuid);
                               auto updateResult = Helpers::upsert(opCtx,
                                                                   _stateDocumentsNS,
                                                                   filter,
                                                                   updatedStateDocBson,
                                                                   /*fromMigrate=*/false);

                               // '$setOnInsert' update operator can never modify an existing
                               // on-disk state doc.
                               invariant(!updateResult.existing);
                               invariant(!updateResult.numDocsModified);

                               return repl::ReplClientInfo::forClient(opCtx->getClient())
                                   .getLastOp();
                           }

                           const auto originalRecordId =
                               Helpers::findOne(opCtx,
                                                collection.getCollection(),
                                                BSON("_id" << originalStateDocBson["_id"]));
                           const auto originalSnapshot = Snapshotted<BSONObj>(
                               opCtx->recoveryUnit()->getSnapshotId(), originalStateDocBson);
                           invariant(!originalRecordId.isNull());

                           CollectionUpdateArgs args;
                           args.criteria = BSON("_id" << uuid);
                           args.oplogSlots = {oplogSlot};
                           args.update = updatedStateDocBson;

                           collection->updateDocument(opCtx,
                                                      originalRecordId,
                                                      originalSnapshot,
                                                      updatedStateDocBson,
                                                      false,
                                                      nullptr /* OpDebug* */,
                                                      &args);

                           return oplogSlot;
                       }();

                       wuow.commit();
                       return updateOpTime;
                   });

               return repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
           })
        .until([](StatusWith<repl::OpTime> swOpTime) { return swOpTime.getStatus().isOK(); })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, token);
}

ExecutorFuture<void> ShardSplitDonorService::DonorStateMachine::_waitForMajorityWriteConcern(
    const ScopedTaskExecutorPtr& executor, repl::OpTime opTime, const CancellationToken& token) {
    return WaitForMajorityService::get(_serviceContext)
        .waitUntilMajority(std::move(opTime), token)
        .thenRunOn(**executor)
        .then([this, self = shared_from_this()] {
            pauseShardSplitAfterMarkingStateGarbageCollectable.pauseWhileSet();
        });
}

void ShardSplitDonorService::DonorStateMachine::_initiateTimeout(
    const ScopedTaskExecutorPtr& executor, const CancellationToken& abortToken) {
    auto timeoutFuture =
        (*executor)->sleepFor(Milliseconds(repl::shardSplitTimeoutMS.load()), abortToken);

    auto timeoutOrCompletionFuture =
        whenAny(std::move(timeoutFuture),
                decisionFuture().semi().ignoreValue().thenRunOn(**executor))
            .thenRunOn(**executor)
            .then([this, executor, abortToken, anchor = shared_from_this()](auto result) {
                stdx::lock_guard<Latch> lg(_mutex);
                if (_stateDoc.getState() != ShardSplitDonorStateEnum::kCommitted &&
                    _stateDoc.getState() != ShardSplitDonorStateEnum::kAborted &&
                    !abortToken.isCanceled()) {
                    LOGV2(6236500,
                          "Timeout expired, aborting shard split.",
                          "id"_attr = _migrationId,
                          "timeout"_attr = repl::shardSplitTimeoutMS.load());
                    _abortReason = Status(ErrorCodes::ExceededTimeLimit,
                                          "Aborting shard split as it exceeded its time limit.");
                    _abortSource->cancel();
                }
            })
            .semi();
}

ExecutorFuture<ShardSplitDonorService::DonorStateMachine::DurableState>
ShardSplitDonorService::DonorStateMachine::_handleErrorOrEnterAbortedState(
    Status status,
    const ScopedTaskExecutorPtr& executor,
    const CancellationToken& primaryToken,
    const CancellationToken& abortToken) {
    ON_BLOCK_EXIT([&] {
        stdx::lock_guard<Latch> lg(_mutex);
        if (_abortSource) {
            // Cancel source to ensure all child threads (RSM monitor, etc)
            // terminate.
            _abortSource->cancel();
        }
    });

    {
        stdx::lock_guard<Latch> lg(_mutex);
        if (isAbortedDocumentPersistent(lg, _stateDoc)) {
            // The document is already in aborted state. No need to write it.
            return ExecutorFuture(**executor,
                                  DurableState{ShardSplitDonorStateEnum::kAborted, _abortReason});
        }
    }

    if (ErrorCodes::isNotPrimaryError(status) || ErrorCodes::isShutdownError(status)) {
        // Don't abort the split on retriable errors that may have been generated by the local
        // server shutting/stepping down because it can be resumed when the client retries.
        return ExecutorFuture(**executor, StatusWith<DurableState>{status});
    }

    // Make sure we don't change the status if the abortToken is cancelled due to a POS instance
    // interruption.
    if (abortToken.isCanceled() && !primaryToken.isCanceled()) {
        status =
            Status(ErrorCodes::TenantMigrationAborted, "Aborted due to 'abortShardSplit' command.");
    }

    {
        stdx::lock_guard<Latch> lg(_mutex);
        if (!_abortReason) {
            _abortReason = status;
        }

        BSONObjBuilder bob;
        _abortReason->serializeErrorToBSON(&bob);
        _stateDoc.setAbortReason(bob.obj());

        LOGV2(6086508,
              "Entering 'aborted' state.",
              "id"_attr = _migrationId,
              "abortReason"_attr = _abortReason.get());
    }

    return ExecutorFuture<void>(**executor)
        .then([this, executor, primaryToken] {
            return _updateStateDocument(executor, primaryToken, ShardSplitDonorStateEnum::kAborted);
        })
        .then([this, executor, primaryToken](repl::OpTime opTime) {
            return _waitForMajorityWriteConcern(executor, std::move(opTime), primaryToken);
        })
        .then([this, executor] {
            stdx::lock_guard<Latch> lg(_mutex);
            return DurableState{_stateDoc.getState(), _abortReason};
        });
}

ExecutorFuture<void>
ShardSplitDonorService::DonorStateMachine::_waitForForgetCmdThenMarkGarbageCollectable(
    const ScopedTaskExecutorPtr& executor, const CancellationToken& primaryToken) {
    stdx::lock_guard<Latch> lg(_mutex);
    if (_stateDoc.getExpireAt() || _stateDoc.getState() < ShardSplitDonorStateEnum::kCommitted) {
        return ExecutorFuture(**executor);
    }

    LOGV2(6236603, "Waiting to receive 'forgetShardSplit' command.", "id"_attr = _migrationId);

    return future_util::withCancellation(_forgetShardSplitReceivedPromise.getFuture(), primaryToken)
        .thenRunOn(**executor)
        .then([this, self = shared_from_this(), executor, primaryToken] {
            LOGV2(6236606, "Marking shard split as garbage-collectable.", "id"_attr = _migrationId);

            stdx::lock_guard<Latch> lg(_mutex);
            _stateDoc.setExpireAt(_serviceContext->getFastClockSource()->now() +
                                  Milliseconds{repl::shardSplitGarbageCollectionDelayMS.load()});

            return AsyncTry([this, self = shared_from_this()] {
                       auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                       uassertStatusOK(serverless::updateStateDoc(opCtx.get(), _stateDoc));
                       return repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
                   })
                .until(
                    [](StatusWith<repl::OpTime> swOpTime) { return swOpTime.getStatus().isOK(); })
                .withBackoffBetweenIterations(kExponentialBackoff)
                .on(**executor, primaryToken);
        })
        .then([this, self = shared_from_this(), executor, primaryToken](repl::OpTime opTime) {
            return _waitForMajorityWriteConcern(executor, std::move(opTime), primaryToken);
        });
}

ExecutorFuture<void> ShardSplitDonorService::DonorStateMachine::_removeSplitConfigFromDonor(
    const ScopedTaskExecutorPtr& executor, const CancellationToken& token) {
    checkForTokenInterrupt(token);

    auto replCoord = repl::ReplicationCoordinator::get(cc().getServiceContext());
    invariant(replCoord);

    return AsyncTry([this, replCoord] {
               auto config = replCoord->getConfig();
               if (!config.isSplitConfig()) {
                   return;
               }

               LOGV2(6573000,
                     "Reconfiguring the donor to remove the split config.",
                     "id"_attr = _migrationId,
                     "config"_attr = config);

               BSONObjBuilder newConfigBob(
                   config.toBSON().removeField("recipientConfig").removeField("version"));
               newConfigBob.append("version", config.getConfigVersion() + 1);

               auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
               DBDirectClient client(opCtx.get());

               BSONObj result;
               const bool returnValue =
                   client.runCommand(NamespaceString::kAdminDb.toString(),
                                     BSON("replSetReconfig" << newConfigBob.obj()),
                                     result);
               uassert(
                   ErrorCodes::BadValue, "Invalid return value for replSetReconfig", returnValue);
               uassertStatusOK(getStatusFromCommandResult(result));
           })
        .until([](Status status) { return status.isOK(); })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, token);
}

ExecutorFuture<void> ShardSplitDonorService::DonorStateMachine::_cleanRecipientStateDoc(
    const ScopedTaskExecutorPtr& executor, const CancellationToken& primaryToken) {
    LOGV2(6309000, "Cleaning up shard split operation on recipient.", "id"_attr = _migrationId);
    return AsyncTry([this, self = shared_from_this()] {
               auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
               auto deleted =
                   uassertStatusOK(serverless::deleteStateDoc(opCtx.get(), _migrationId));
               uassert(ErrorCodes::ConflictingOperationInProgress,
                       str::stream()
                           << "Did not find active shard split with migration id " << _migrationId,
                       deleted);
               return repl::ReplClientInfo::forClient(opCtx.get()->getClient()).getLastOp();
           })
        .until([](StatusWith<repl::OpTime> swOpTime) { return swOpTime.getStatus().isOK(); })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, primaryToken)
        .ignoreValue();
}
}  // namespace mongo
