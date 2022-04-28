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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTenantMigration


#include "mongo/db/serverless/shard_split_donor_service.h"
#include "mongo/client/streamable_replica_set_monitor.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/db/repl/wait_for_majority_service.h"
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

namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(pauseShardSplitAfterBlocking);
MONGO_FAIL_POINT_DEFINE(pauseShardSplitAfterDecision);
MONGO_FAIL_POINT_DEFINE(skipShardSplitWaitForSplitAcceptance);
MONGO_FAIL_POINT_DEFINE(pauseShardSplitBeforeRecipientCleanup);

const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());

bool shouldStopInsertingDonorStateDoc(Status status) {
    return status.isOK() || status == ErrorCodes::ConflictingOperationInProgress;
}

void setStateDocTimestamps(WithLock,
                           ShardSplitDonorStateEnum nextState,
                           repl::OpTime time,
                           ShardSplitDonorDocument& stateDoc) {
    switch (nextState) {
        case ShardSplitDonorStateEnum::kUninitialized:
            break;
        case ShardSplitDonorStateEnum::kBlocking:
            stateDoc.setBlockTimestamp(time.getTimestamp());
            break;
        case ShardSplitDonorStateEnum::kAborted:
            stateDoc.setCommitOrAbortOpTime(time);
            break;
        case ShardSplitDonorStateEnum::kCommitted:
            stateDoc.setCommitOrAbortOpTime(time);
            break;
        default:
            MONGO_UNREACHABLE;
    }
}

bool isAbortedDocumentPersistent(WithLock, ShardSplitDonorDocument& stateDoc) {
    return !!stateDoc.getAbortReason();
}

void setMtabToBlockingForTenants(ServiceContext* context,
                                 OperationContext* opCtx,
                                 const std::vector<StringData>& tenantIds) {
    // Start blocking writes before getting an oplog slot to guarantee no
    // writes to the tenant's data can commit with a timestamp after the
    // block timestamp.
    for (const auto& tenantId : tenantIds) {
        auto mtab = tenant_migration_access_blocker::getTenantMigrationDonorAccessBlocker(context,
                                                                                          tenantId);
        invariant(mtab);
        mtab->startBlockingWrites();

        opCtx->recoveryUnit()->onRollback([mtab] { mtab->rollBackStartBlocking(); });
    }
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
    const CancellationToken& token,
    const ConnectionString& recipientConnectionString) {

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

    LOGV2(6142508,
          "Monitoring recipient nodes for split acceptance.",
          "recipientConnectionString"_attr = recipientConnectionString);

    return future_util::withCancellation(listener->getFuture(), token)
        .thenRunOn(taskExecutor)
        // Preserve lifetime of listener and monitor until the future is fulfilled and remove the
        // listener.
        .onCompletion(
            [monitors = std::move(monitors), listener, eventsPublisher, taskExecutor](Status s) {
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
    auto stateDoc =
        ShardSplitDonorDocument::parse(IDLParserErrorContext("donorStateDoc"), initialState);

    for (auto& instance : existingInstances) {
        auto existingTypedInstance =
            checked_cast<const ShardSplitDonorService::DonorStateMachine*>(instance);
        bool isGarbageCollectable = existingTypedInstance->isGarbageCollectable();
        bool existingIsAborted =
            existingTypedInstance->getStateDocState() == ShardSplitDonorStateEnum::kAborted &&
            isGarbageCollectable;

        uassert(ErrorCodes::ConflictingOperationInProgress,
                str::stream() << "Can't start a concurent shard split operation against"
                              << " migrationId:" << existingTypedInstance->getId(),
                existingIsAborted);
    }
}

std::shared_ptr<repl::PrimaryOnlyService::Instance> ShardSplitDonorService::constructInstance(
    BSONObj initialState) {
    return std::make_shared<DonorStateMachine>(
        _serviceContext,
        this,
        ShardSplitDonorDocument::parse(IDLParserErrorContext("donorStateDoc"), initialState));
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
    LOGV2(6086502, "Aborting shard split", "id"_attr = _migrationId);
    stdx::lock_guard<Latch> lg(_mutex);
    _abortRequested = true;
    if (_abortSource) {
        _abortSource->cancel();
    }
}

void ShardSplitDonorService::DonorStateMachine::tryForget() {
    LOGV2(6236601, "Forgetting shard split", "id"_attr = _migrationId);

    stdx::lock_guard<Latch> lg(_mutex);
    if (_forgetShardSplitReceivedPromise.getFuture().isReady()) {
        LOGV2(6236602, "Donor Forget Migration promise is already ready", "id"_attr = _migrationId);
        return;
    }
    _forgetShardSplitReceivedPromise.emplaceValue();
}

Status ShardSplitDonorService::DonorStateMachine::checkIfOptionsConflict(
    const ShardSplitDonorDocument& stateDoc) const {
    stdx::lock_guard<Latch> lg(_mutex);
    invariant(stateDoc.getId() == _stateDoc.getId());

    if (_stateDoc.getTenantIds() == stateDoc.getTenantIds() &&
        _stateDoc.getRecipientTagName() == stateDoc.getRecipientTagName() &&
        _stateDoc.getRecipientSetName() == stateDoc.getRecipientSetName()) {
        return Status::OK();
    }

    return Status(ErrorCodes::ConflictingOperationInProgress,
                  str::stream() << "Found active migration for migrationId \""
                                << _stateDoc.getId().toBSON() << "\" with different options "
                                << _stateDoc.toBSON());
}

SemiFuture<void> ShardSplitDonorService::DonorStateMachine::run(
    ScopedTaskExecutorPtr executor, const CancellationToken& primaryToken) noexcept {
    auto abortToken = [&]() {
        stdx::lock_guard<Latch> lg(_mutex);
        _abortSource = CancellationSource(primaryToken);
        if (_abortRequested || _stateDoc.getState() == ShardSplitDonorStateEnum::kAborted) {
            _abortSource->cancel();
        }

        return _abortSource->token();
    }();

    _markKilledExecutor->startup();
    _cancelableOpCtxFactory.emplace(primaryToken, _markKilledExecutor);

    pauseShardSplitBeforeRecipientCleanup.pauseWhileSet();

    const bool shouldRemoveStateDocumentOnRecipient = [&]() {
        auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
        stdx::lock_guard<Latch> lg(_mutex);
        return serverless::shouldRemoveStateDocumentOnRecipient(opCtx.get(), _stateDoc);
    }();

    if (shouldRemoveStateDocumentOnRecipient) {
        LOGV2(6309000,
              "Cancelling and cleaning up shard split operation on recipient in blocking state.",
              "id"_attr = _migrationId);
        _decisionPromise.setWith([&] {
            return ExecutorFuture(**executor)
                .then([this, executor, primaryToken, anchor = shared_from_this()] {
                    return _cleanRecipientStateDoc(executor, primaryToken);
                })
                .then([this, executor, migrationId = _migrationId]() {
                    LOGV2(6236607,
                          "Cleanup stale shard split operation on recipient.",
                          "migrationId"_attr = migrationId);
                    return DurableState{ShardSplitDonorStateEnum::kCommitted};
                })
                .unsafeToInlineFuture();
        });

        _completionPromise.setWith([&] {
            return _decisionPromise.getFuture().semi().ignoreValue().unsafeToInlineFuture();
        });

        return _completionPromise.getFuture().semi();
    }

    auto isConfigValidWithStatus = [&]() {
        auto replCoord = repl::ReplicationCoordinator::get(cc().getServiceContext());
        invariant(replCoord);
        stdx::lock_guard<Latch> lg(_mutex);
        return serverless::validateRecipientNodesForShardSplit(_stateDoc, replCoord->getConfig());
    }();
    if (!isConfigValidWithStatus.isOK()) {
        LOGV2_ERROR(6395900,
                    "Failed to validate recipient nodes for shard split",
                    "id"_attr = _migrationId,
                    "error"_attr = isConfigValidWithStatus.reason());
        _decisionPromise.emplaceValue(DurableState{ShardSplitDonorStateEnum::kCommitted});
        _completionPromise.setFrom(
            _decisionPromise.getFuture().semi().ignoreValue().unsafeToInlineFuture());
        return _completionPromise.getFuture().semi();
    }

    _initiateTimeout(executor, abortToken);

    LOGV2(6086506,
          "Starting shard split.",
          "id"_attr = _migrationId,
          "timeout"_attr = repl::shardSplitTimeoutMS.load());

    _decisionPromise.setWith([&] {
        return ExecutorFuture(**executor)
            .then([this, executor, primaryToken] {
                // Note we do not use the abort split token here because the abortShardSplit
                // command waits for a decision to be persisted which will not happen if
                // inserting the initial state document fails.
                return _enterBlockingOrAbortedState(executor, primaryToken);
            })
            .then([this, executor, abortToken] {
                checkForTokenInterrupt(abortToken);
                _cancelableOpCtxFactory.emplace(abortToken, _markKilledExecutor);
                if (MONGO_unlikely(pauseShardSplitAfterBlocking.shouldFail())) {
                    auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                    pauseShardSplitAfterBlocking.pauseWhileSetAndNotCanceled(opCtx.get(),
                                                                             abortToken);
                }

                return _waitForRecipientToReachBlockTimestamp(executor, abortToken);
            })
            .then([this, executor, abortToken] {
                return _applySplitConfigToDonor(executor, abortToken);
            })
            .then([this, executor, abortToken] {
                return _waitForRecipientToAcceptSplit(executor, abortToken);
            })
            .then([this, executor, abortToken] {
                LOGV2(6086503,
                      "Shard split completed",
                      "id"_attr = _migrationId,
                      "abortReason"_attr = _abortReason);

                stdx::lock_guard<Latch> lg(_mutex);
                return DurableState{_stateDoc.getState(), _abortReason};
            })
            // anchor ensures the instance will still exists even if the primary stepped down
            .onError([this, executor, primaryToken, abortToken, anchor = shared_from_this()](
                         StatusWith<DurableState> statusWithState) {
                // only cancel operations on stepdown from here out
                _cancelableOpCtxFactory.emplace(primaryToken, _markKilledExecutor);

                return _handleErrorOrEnterAbortedState(
                    statusWithState, executor, primaryToken, abortToken);
            })
            .unsafeToInlineFuture();
    });

    _completionPromise.setFrom(
        _decisionPromise.getFuture()
            .semi()
            .ignoreValue()
            .thenRunOn(**executor)
            .then([this, anchor = shared_from_this(), executor, primaryToken] {
                auto opCtx = _cancelableOpCtxFactory->makeOperationContext(&cc());
                pauseShardSplitAfterDecision.pauseWhileSetAndNotCanceled(opCtx.get(), primaryToken);
                return _waitForForgetCmdThenMarkGarbageCollectible(executor, primaryToken);
            })
            .unsafeToInlineFuture());

    return _completionPromise.getFuture().semi();
}

void ShardSplitDonorService::DonorStateMachine::interrupt(Status status) {}

boost::optional<BSONObj> ShardSplitDonorService::DonorStateMachine::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode connMode,
    MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept {

    stdx::lock_guard<Latch> lg(_mutex);
    BSONObjBuilder bob;
    bob.append("desc", "shard split operation");
    return bob.obj();
}

ExecutorFuture<void>
ShardSplitDonorService::DonorStateMachine::_waitForRecipientToReachBlockTimestamp(
    const ScopedTaskExecutorPtr& executor, const CancellationToken& abortToken) {
    checkForTokenInterrupt(abortToken);

    stdx::lock_guard<Latch> lg(_mutex);
    if (_stateDoc.getState() > ShardSplitDonorStateEnum::kBlocking) {
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
    const ScopedTaskExecutorPtr& executor, const CancellationToken& token) {
    checkForTokenInterrupt(token);

    {
        stdx::lock_guard<Latch> lg(_mutex);
        if (_stateDoc.getState() >= ShardSplitDonorStateEnum::kCommitted) {
            return ExecutorFuture(**executor);
        }
    }


    auto replCoord = repl::ReplicationCoordinator::get(cc().getServiceContext());
    invariant(replCoord);

    LOGV2(6309100,
          "Generating and applying a split config",
          "id"_attr = _migrationId,
          "conf"_attr = replCoord->getConfig());

    return AsyncTry([this] {
               auto opCtxHolder = _cancelableOpCtxFactory->makeOperationContext(&cc());

               auto newConfig = [&]() {
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

               DBDirectClient client(opCtxHolder.get());

               BSONObj result;
               const bool returnValue =
                   client.runCommand(NamespaceString::kAdminDb.toString(),
                                     BSON("replSetReconfig" << newConfig.toBSON()),
                                     result);
               uassert(
                   ErrorCodes::BadValue, "Invalid return value for replSetReconfig", returnValue);
               uassertStatusOK(getStatusFromCommandResult(result));
           })
        .until([](Status status) { return status.isOK(); })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, token)
        .then([this] {
            LOGV2(6309101,
                  "Split config has been generated and committed.",
                  "id"_attr = _migrationId);
        });
}

ExecutorFuture<void> ShardSplitDonorService::DonorStateMachine::_waitForRecipientToAcceptSplit(
    const ScopedTaskExecutorPtr& executor, const CancellationToken& token) {
    checkForTokenInterrupt(token);

    {
        stdx::lock_guard<Latch> lg(_mutex);
        if (_stateDoc.getState() > ShardSplitDonorStateEnum::kBlocking) {
            return ExecutorFuture(**executor);
        }

        LOGV2(6142501, "Waiting for recipient to accept the split.", "id"_attr = _migrationId);
    }

    return ExecutorFuture(**executor)
        .then([&]() { return _splitAcceptancePromise.getFuture(); })
        .then([this, executor, token] {
            LOGV2(6142503,
                  "Recipient has accepted the split, committing decision.",
                  "id"_attr = _migrationId);

            return _updateStateDocument(executor, token, ShardSplitDonorStateEnum::kCommitted)
                .then([this, executor, token](repl::OpTime opTime) {
                    return _waitForMajorityWriteConcern(executor, std::move(opTime), token);
                });
        });
}

ExecutorFuture<void> ShardSplitDonorService::DonorStateMachine::_enterBlockingOrAbortedState(
    const ScopedTaskExecutorPtr& executor, const CancellationToken& primaryServiceToken) {

    ShardSplitDonorStateEnum nextState;
    {
        stdx::lock_guard<Latch> lg(_mutex);
        if (_stateDoc.getState() == ShardSplitDonorStateEnum::kAborted) {
            if (isAbortedDocumentPersistent(lg, _stateDoc)) {
                // Node has step up and created an instance using a document in abort state. No need
                // to write the document as it already exists.
                return ExecutorFuture(**executor);
            }

            _abortReason =
                Status(ErrorCodes::TenantMigrationAborted, "Aborted due to abortShardSplit.");
            BSONObjBuilder bob;
            _abortReason->serializeErrorToBSON(&bob);
            _stateDoc.setAbortReason(bob.obj());
            _stateDoc.setExpireAt(_serviceContext->getFastClockSource()->now() +
                                  Milliseconds{repl::shardSplitGarbageCollectionDelayMS.load()});
            nextState = ShardSplitDonorStateEnum::kAborted;
        } else {
            auto recipientTagName = _stateDoc.getRecipientTagName();
            invariant(recipientTagName);
            auto recipientSetName = _stateDoc.getRecipientSetName();
            invariant(recipientSetName);
            auto config = repl::ReplicationCoordinator::get(cc().getServiceContext())->getConfig();
            auto recipientConnectionString = serverless::makeRecipientConnectionString(
                config, *recipientTagName, *recipientSetName);

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

                return detail::makeRecipientAcceptSplitFuture(
                           executor, primaryServiceToken, recipientConnectionString)
                    .unsafeToInlineFuture();
            });

            if (_stateDoc.getState() > ShardSplitDonorStateEnum::kUninitialized) {
                // Node has step up and resumed a shard split. No need to write the document as it
                // already exists.
                return ExecutorFuture(**executor);
            }

            // Otherwise, record the recipient connection string
            _stateDoc.setRecipientConnectionString(recipientConnectionString);
            _stateDoc.setState(ShardSplitDonorStateEnum::kBlocking);
            nextState = ShardSplitDonorStateEnum::kBlocking;
        }
    }

    LOGV2(6086504,
          "Inserting initial state document.",
          "id"_attr = _migrationId,
          "state"_attr = nextState);

    return AsyncTry([this, nextState, uuid = _migrationId]() {
               auto opCtxHolder = _cancelableOpCtxFactory->makeOperationContext(&cc());
               auto opCtx = opCtxHolder.get();

               AutoGetCollection collection(opCtx, _stateDocumentsNS, MODE_IX);

               writeConflictRetry(
                   opCtx, "ShardSplitDonorInsertStateDoc", _stateDocumentsNS.ns(), [&] {
                       const auto filter = BSON(ShardSplitDonorDocument::kIdFieldName << uuid);
                       const auto getUpdatedStateDocBson = [&]() {
                           stdx::lock_guard<Latch> lg(_mutex);
                           return _stateDoc.toBSON();
                       };

                       WriteUnitOfWork wuow(opCtx);
                       if (nextState == ShardSplitDonorStateEnum::kBlocking) {
                           stdx::lock_guard<Latch> lg(_mutex);

                           insertTenantAccessBlocker(lg, opCtx, _stateDoc);

                           auto tenantIds = _stateDoc.getTenantIds();
                           invariant(tenantIds);
                           setMtabToBlockingForTenants(_serviceContext, opCtx, tenantIds.get());
                       }

                       // Reserve an opTime for the write.
                       auto oplogSlot = LocalOplogInfo::get(opCtx)->getNextOpTimes(opCtx, 1U)[0];
                       setStateDocTimestamps(
                           stdx::lock_guard<Latch>{_mutex}, nextState, oplogSlot, _stateDoc);

                       auto updateResult = Helpers::upsert(opCtx,
                                                           _stateDocumentsNS.ns(),
                                                           filter,
                                                           getUpdatedStateDocBson(),
                                                           /*fromMigrate=*/false);


                       // We only want to insert, not modify, document
                       invariant(updateResult.numMatched == 0);
                       wuow.commit();
                   });

               return repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
           })
        .until([](StatusWith<repl::OpTime> swOpTime) {
            return shouldStopInsertingDonorStateDoc(swOpTime.getStatus());
        })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, primaryServiceToken)
        .then([this, executor, primaryServiceToken](repl::OpTime opTime) {
            return _waitForMajorityWriteConcern(executor, std::move(opTime), primaryServiceToken);
        })
        .then([this, executor, nextState]() {
            uassert(ErrorCodes::TenantMigrationAborted,
                    "Shard split operation aborted",
                    nextState != ShardSplitDonorStateEnum::kAborted);
        });
}

ExecutorFuture<repl::OpTime> ShardSplitDonorService::DonorStateMachine::_updateStateDocument(
    const ScopedTaskExecutorPtr& executor,
    const CancellationToken& token,
    ShardSplitDonorStateEnum nextState) {
    auto tenantIds = [&]() {
        stdx::lock_guard<Latch> lg(_mutex);
        _stateDoc.setState(nextState);

        return _stateDoc.getTenantIds();
    }();

    return AsyncTry([this, tenantIds = std::move(tenantIds), uuid = _migrationId, nextState] {
               auto opCtxHolder = _cancelableOpCtxFactory->makeOperationContext(&cc());
               auto opCtx = opCtxHolder.get();

               AutoGetCollection collection(opCtx, _stateDocumentsNS, MODE_IX);
               uassert(ErrorCodes::NamespaceNotFound,
                       str::stream() << _stateDocumentsNS.ns() << " does not exist",
                       collection);

               writeConflictRetry(
                   opCtx, "ShardSplitDonorUpdateStateDoc", _stateDocumentsNS.ns(), [&] {
                       WriteUnitOfWork wuow(opCtx);

                       // Reserve an opTime for the write.
                       auto oplogSlot = LocalOplogInfo::get(opCtx)->getNextOpTimes(opCtx, 1U)[0];
                       setStateDocTimestamps(
                           stdx::lock_guard<Latch>{_mutex}, nextState, oplogSlot, _stateDoc);

                       const auto filter = BSON(ShardSplitDonorDocument::kIdFieldName << uuid);
                       const auto updatedStateDocBson = [&]() {
                           stdx::lock_guard<Latch> lg(_mutex);
                           return BSON("$set" << _stateDoc.toBSON());
                       }();
                       auto updateResult = Helpers::upsert(opCtx,
                                                           _stateDocumentsNS.ns(),
                                                           filter,
                                                           updatedStateDocBson,
                                                           /*fromMigrate=*/false);

                       invariant(updateResult.numDocsModified == 1);
                       wuow.commit();
                   });

               return repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
           })
        .until([](StatusWith<repl::OpTime> swOpTime) {
            return shouldStopInsertingDonorStateDoc(swOpTime.getStatus());
        })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, token);
}

ExecutorFuture<void> ShardSplitDonorService::DonorStateMachine::_waitForMajorityWriteConcern(
    const ScopedTaskExecutorPtr& executor, repl::OpTime opTime, const CancellationToken& token) {
    return WaitForMajorityService::get(_serviceContext)
        .waitUntilMajority(std::move(opTime), token)
        .thenRunOn(**executor);
}

void ShardSplitDonorService::DonorStateMachine::_initiateTimeout(
    const ScopedTaskExecutorPtr& executor, const CancellationToken& abortToken) {

    auto timeoutFuture =
        (*executor)->sleepFor(Milliseconds(repl::shardSplitTimeoutMS.load()), abortToken);

    auto timeoutOrCompletionFuture =
        whenAny(std::move(timeoutFuture),
                completionFuture().semi().ignoreValue().thenRunOn(**executor))
            .thenRunOn(**executor)
            .then([this, executor, anchor = shared_from_this()](auto result) {
                stdx::lock_guard<Latch> lg(_mutex);
                if (_stateDoc.getState() != ShardSplitDonorStateEnum::kCommitted &&
                    _stateDoc.getState() != ShardSplitDonorStateEnum::kAborted &&
                    !_abortRequested) {
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
    StatusWith<DurableState> statusWithState,
    const ScopedTaskExecutorPtr& executor,
    const CancellationToken& primaryToken,
    const CancellationToken& abortToken) {
    {
        stdx::lock_guard<Latch> lg(_mutex);
        if (isAbortedDocumentPersistent(lg, _stateDoc)) {
            // The document is already in aborted state. No need to write it.
            return ExecutorFuture(**executor,
                                  DurableState{ShardSplitDonorStateEnum::kAborted, _abortReason});
        }
    }

    const auto status = statusWithState.getStatus();
    if (ErrorCodes::isNotPrimaryError(status) || ErrorCodes::isShutdownError(status)) {
        // Don't abort the split on retriable errors that may have been generated by the local
        // server shutting/stepping down because it can be resumed when the client retries.

        return ExecutorFuture(**executor, statusWithState);
    }

    // There is no use to check the parent token the executor would not run if the parent token
    // is cancelled. At this point either the abortToken has been cancelled or a previous
    // operation failed. In either case we abort the migration.
    if (abortToken.isCanceled()) {
        statusWithState =
            Status(ErrorCodes::TenantMigrationAborted, "Aborted due to abortShardSplit.");
    }

    {
        stdx::lock_guard<Latch> lg(_mutex);

        if (!_abortReason) {
            _abortReason = statusWithState.getStatus();
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
ShardSplitDonorService::DonorStateMachine::_waitForForgetCmdThenMarkGarbageCollectible(
    const ScopedTaskExecutorPtr& executor, const CancellationToken& token) {
    LOGV2(6236603,
          "Waiting to receive 'forgetShardSplit' command.",
          "migrationId"_attr = _migrationId);
    auto expiredAt = [&]() {
        stdx::lock_guard<Latch> lg(_mutex);
        return _stateDoc.getExpireAt();
    }();

    if (expiredAt) {
        LOGV2(6236604, "expiredAt is already set", "migrationId"_attr = _migrationId);
        return ExecutorFuture(**executor);
    }

    return future_util::withCancellation(_forgetShardSplitReceivedPromise.getFuture(), token)
        .thenRunOn(**executor)
        .then([this, self = shared_from_this(), executor, token] {
            LOGV2(6236606,
                  "Marking shard split as garbage-collectable.",
                  "migrationId"_attr = _migrationId);

            stdx::lock_guard<Latch> lg(_mutex);
            _stateDoc.setExpireAt(_serviceContext->getFastClockSource()->now() +
                                  Milliseconds{repl::shardSplitGarbageCollectionDelayMS.load()});

            return AsyncTry([this, self = shared_from_this()] {
                       auto opCtxHolder = cc().makeOperationContext();
                       auto opCtx = opCtxHolder.get();

                       uassertStatusOK(serverless::updateStateDoc(opCtx, _stateDoc));
                       return repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
                   })
                .until(
                    [](StatusWith<repl::OpTime> swOpTime) { return swOpTime.getStatus().isOK(); })
                .withBackoffBetweenIterations(kExponentialBackoff)
                .on(**executor, token);
        })
        .then([this, self = shared_from_this(), executor, token](repl::OpTime opTime) {
            return _waitForMajorityWriteConcern(executor, std::move(opTime), token);
        });
}

ExecutorFuture<void> ShardSplitDonorService::DonorStateMachine::_cleanRecipientStateDoc(
    const ScopedTaskExecutorPtr& executor, const CancellationToken& token) {

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
        .on(**executor, token)
        .ignoreValue();
}
}  // namespace mongo
