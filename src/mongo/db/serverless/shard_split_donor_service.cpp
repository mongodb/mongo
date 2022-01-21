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
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/executor/cancelable_executor.h"
#include "mongo/executor/connection_pool.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_util.h"
#include "mongo/util/time_support.h"

namespace mongo {

namespace {

const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());

bool shouldStopInsertingDonorStateDoc(Status status) {
    return status.isOK() || status == ErrorCodes::ConflictingOperationInProgress;
}

void setStateDocTimestamps(WithLock,
                           ShardSplitDonorStateEnum nextState,
                           repl::OpTime time,
                           ShardSplitDonorDocument& stateDoc) {
    switch (nextState) {
        case ShardSplitDonorStateEnum::kDataSync:
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

MONGO_FAIL_POINT_DEFINE(pauseShardSplitAfterInitialSync);
MONGO_FAIL_POINT_DEFINE(pauseShardSplitAfterBlocking);

}  // namespace

std::function<bool(const std::vector<sdam::ServerDescriptionPtr>&)>
makeRecipientAcceptSplitPredicate(std::string name, int expectedSize) {
    return [name = std::move(name),
            expectedSize](const std::vector<sdam::ServerDescriptionPtr>& servers) {
        return expectedSize ==
            std::count_if(servers.begin(), servers.end(), [&](const auto& server) {
                   return server->getSetName() && *(server->getSetName()) == name;
               });
    };
}

SemiFuture<void> getRecipientAcceptSplitFuture(ExecutorPtr executor,
                                               const CancellationToken& token,
                                               MongoURI recipientConnectionString) {
    class RecipientAcceptSplitListener : public sdam::TopologyListener {
    public:
        RecipientAcceptSplitListener(int expectedSize, std::string rsName)
            : _predicate(makeRecipientAcceptSplitPredicate(std::move(rsName), expectedSize)) {}
        void onTopologyDescriptionChangedEvent(TopologyDescriptionPtr previousDescription,
                                               TopologyDescriptionPtr newDescription) final {
            stdx::lock_guard<Latch> lg(_mutex);
            if (_fulfilled) {
                return;
            }

            if (_predicate(newDescription->getServers())) {
                _fulfilled = true;
                _promise.emplaceValue();
            }
        }

        // Fulfilled when all nodes have accepted the split.
        SharedSemiFuture<void> getFuture() const {
            return _promise.getFuture();
        }

    private:
        bool _fulfilled = false;
        std::function<bool(const std::vector<sdam::ServerDescriptionPtr>&)> _predicate;
        SharedPromise<void> _promise;
        mutable Mutex _mutex =
            MONGO_MAKE_LATCH("ShardSplitDonorService::getRecipientAcceptSplitFuture::_mutex");
    };

    auto monitor = ReplicaSetMonitor::createIfNeeded(recipientConnectionString);
    invariant(monitor);

    // TODO SERVER-62079 : Remove check for scanning RSM as it does not exist anymore.
    auto streamableMonitor = std::dynamic_pointer_cast<StreamableReplicaSetMonitor>(monitor);
    uassert(6142507,
            "feature \"shard split\" can only work with a StreamableReplicaSetMonitor.",
            streamableMonitor);

    auto listener = std::make_shared<RecipientAcceptSplitListener>(
        recipientConnectionString.getServers().size(),
        recipientConnectionString.getReplicaSetName());

    streamableMonitor->getEventsPublisher()->registerListener(listener);

    return future_util::withCancellation(listener->getFuture(), token)
        .thenRunOn(executor)
        // Preserve lifetime of listener and monitor until the future is fulfilled and remove the
        // listener.
        .onCompletion([listener, monitor = streamableMonitor](Status s) {
            monitor->getEventsPublisher()->removeListener(listener);
            return s;
        })
        .semi();
}

ThreadPool::Limits ShardSplitDonorService::getThreadPoolLimits() const {
    return ThreadPool::Limits();
}

std::shared_ptr<repl::PrimaryOnlyService::Instance> ShardSplitDonorService::constructInstance(
    BSONObj initialState) {
    return std::make_shared<DonorStateMachine>(
        _serviceContext,
        this,
        ShardSplitDonorDocument::parse(IDLParserErrorContext("donorStateDoc"), initialState));
}

ShardSplitDonorService::DonorStateMachine::DonorStateMachine(
    ServiceContext* serviceContext,
    ShardSplitDonorService* splitService,
    const ShardSplitDonorDocument& initialState)
    : repl::PrimaryOnlyService::TypedInstance<DonorStateMachine>(),
      _migrationId(initialState.getId()),
      _serviceContext(serviceContext),
      _shardSplitService(splitService),
      _stateDoc(initialState) {}

void ShardSplitDonorService::DonorStateMachine::tryAbort() {
    LOGV2(6086502, "Aborting shard split", "id"_attr = _migrationId);
    stdx::lock_guard<Latch> lg(_mutex);
    _abortRequested = true;
    if (_abortSource) {
        _abortSource->cancel();
    }
}

Status ShardSplitDonorService::DonorStateMachine::checkIfOptionsConflict(
    const ShardSplitDonorDocument& stateDoc) const {
    stdx::lock_guard<Latch> lg(_mutex);
    invariant(stateDoc.getId() == _stateDoc.getId());

    if (_stateDoc.getTenantIds() == stateDoc.getTenantIds() &&
        _stateDoc.getRecipientConnectionString() == stateDoc.getRecipientConnectionString()) {
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

    LOGV2(6086506, "Starting shard split {id}", "id"_attr = _migrationId);

    auto cancelableExecutor = CancelableExecutor::make(**executor, abortToken);

    _completionPromise.setWith([&] {
        return ExecutorFuture(**executor)
            .then([this, executor, primaryToken] {
                // Note we do not use the abort split token here because the abortShardSplit
                // command waits for a decision to be persisted which will not happen if
                // inserting the initial state document fails.

                return _writeInitialDocument(executor, primaryToken);
            })
            // Once the initial write is done, use the abort token
            .thenRunOn(cancelableExecutor)
            .then([this, cancelableExecutor, abortToken] {
                _createReplicaSetMonitor(cancelableExecutor, abortToken);
            })
            .then([this, executor, cancelableExecutor, abortToken] {
                // Passing executor as a TaskExecutor is required by the downstream call to
                // AsyncTryUntilWithDelay::on(...)
                return _enterDataSyncState(executor, abortToken).thenRunOn(cancelableExecutor);
            })
            .then([this] { pauseShardSplitAfterInitialSync.pauseWhileSet(); })
            .then([this, executor, cancelableExecutor, abortToken] {
                // Passing executor as a TaskExecutor is required by the downstream call to
                // AsyncTryUntilWithDelay::on(...)
                return _enterBlockingState(executor, abortToken).thenRunOn(cancelableExecutor);
            })
            .then([this] { pauseShardSplitAfterBlocking.pauseWhileSet(); })
            .then([this, executor, cancelableExecutor, abortToken] {
                // Passing executor as a TaskExecutor is required by the downstream call to
                // AsyncTry::on(...)
                return _waitForRecipientToAcceptSplit(executor, abortToken)
                    .thenRunOn(cancelableExecutor);
            })
            .then([this, cancelableExecutor, abortToken] {
                LOGV2(6086503,
                      "Shard split completed",
                      "id"_attr = _migrationId,
                      "abortReason"_attr = _abortReason);

                stdx::lock_guard<Latch> lg(_mutex);

                return DurableState{_stateDoc.getState(), _abortReason};
            })
            .thenRunOn(**executor)
            // anchor ensures the instance will still exists even if the primary stepped down
            .onError([this, executor, primaryToken, abortToken, anchor = shared_from_this()](
                         StatusWith<DurableState> statusWithState) {
                return _handleErrorOrEnterAbortedState(
                    statusWithState, executor, primaryToken, abortToken);
            })
            .unsafeToInlineFuture();
    });

    return _completionPromise.getFuture().semi().ignoreValue();
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

ExecutorFuture<void> ShardSplitDonorService::DonorStateMachine::_enterDataSyncState(
    const ScopedTaskExecutorPtr& executor, const CancellationToken& abortToken) {
    {
        stdx::lock_guard<Latch> lg(_mutex);
        if (_stateDoc.getState() >= ShardSplitDonorStateEnum::kDataSync) {
            return ExecutorFuture(**executor);
        }
    }

    LOGV2(6086507, "Shard split entering data sync state {id}", "id"_attr = _migrationId);

    return _updateStateDocument(executor, abortToken, ShardSplitDonorStateEnum::kDataSync)
        .then([this, executor, abortToken](repl::OpTime opTime) {
            return _waitForMajorityWriteConcern(executor, std::move(opTime), abortToken);
        });
}

ExecutorFuture<void> ShardSplitDonorService::DonorStateMachine::_enterBlockingState(
    const ScopedTaskExecutorPtr& executor, const CancellationToken& abortToken) {
    {
        stdx::lock_guard<Latch> lg(_mutex);
        if (_stateDoc.getState() >= ShardSplitDonorStateEnum::kBlocking) {
            return ExecutorFuture(**executor);
        }
    }

    LOGV2(6177200, "Shard split entering blocking state {id}", "id"_attr = _migrationId);

    return _updateStateDocument(executor, abortToken, ShardSplitDonorStateEnum::kBlocking)
        .then([this, executor, abortToken](repl::OpTime opTime) {
            return _waitForMajorityWriteConcern(executor, std::move(opTime), abortToken);
        });
}

ExecutorFuture<void> ShardSplitDonorService::DonorStateMachine::_waitForRecipientToAcceptSplit(
    const ScopedTaskExecutorPtr& executor, const CancellationToken& token) {

    {
        stdx::lock_guard<Latch> lg(_mutex);
        if (_stateDoc.getState() > ShardSplitDonorStateEnum::kBlocking) {
            return ExecutorFuture(**executor);
        }

        LOGV2(6142501, "Waiting for recipient to accept the split.");
    }

    return _recipientAcceptedSplit.getFuture().thenRunOn(**executor).then([this, executor, token] {
        LOGV2(6142503, "Recipient has accepted the split, committing shard split decision");

        return _updateStateDocument(executor, token, ShardSplitDonorStateEnum::kCommitted)
            .then([this, executor, token](repl::OpTime opTime) {
                return _waitForMajorityWriteConcern(executor, std::move(opTime), token);
            });
    });
}

ExecutorFuture<void> ShardSplitDonorService::DonorStateMachine::_writeInitialDocument(
    const ScopedTaskExecutorPtr& executor, const CancellationToken& primaryServiceToken) {

    const auto initialState = [&]() {
        stdx::lock_guard<Latch> lg(_mutex);
        return _stateDoc.getState();
    }();

    if (initialState == ShardSplitDonorStateEnum::kAborted) {
        stdx::lock_guard<Latch> lg(_mutex);
        if (isAbortedDocumentPersistent(lg, _stateDoc)) {
            // Node has step up and created an instance using a document in abort state. No need to
            // write the document as it already exists.
            return ExecutorFuture(**executor);
        }

        _abortReason =
            Status(ErrorCodes::TenantMigrationAborted, "Aborted due to abortShardSplit.");
        BSONObjBuilder bob;
        _abortReason->serializeErrorToBSON(&bob);
        _stateDoc.setAbortReason(bob.obj());

    } else if (initialState != ShardSplitDonorStateEnum::kUninitialized) {
        // Node has step up and resumed a shard split. No need to write the document as it already
        // exists.
        return ExecutorFuture(**executor);
    }

    LOGV2(6086504,
          "Inserting initial state document {id} {state}",
          "id"_attr = _migrationId,
          "state"_attr = initialState);

    return AsyncTry([this, nextState = initialState, uuid = _migrationId]() {
               auto opCtxHolder = cc().makeOperationContext();
               auto opCtx = opCtxHolder.get();

               AutoGetCollection collection(opCtx, _stateDocumentsNS, MODE_IX);

               writeConflictRetry(
                   opCtx, "ShardSplitDonorInsertStateDoc", _stateDocumentsNS.ns(), [&] {
                       const auto filter = BSON(ShardSplitDonorDocument::kIdFieldName << uuid);
                       const auto getUpdatedStateDocBson = [&]() {
                           stdx::lock_guard<Latch> lg(_mutex);
                           return _stateDoc.toBSON();
                       };

                       if (nextState == ShardSplitDonorStateEnum::kAborted) {
                           WriteUnitOfWork wuow(opCtx);

                           // Reserve an opTime for the write.
                           auto oplogSlot =
                               LocalOplogInfo::get(opCtx)->getNextOpTimes(opCtx, 1U)[0];
                           setStateDocTimestamps(
                               stdx::lock_guard<Latch>{_mutex}, nextState, oplogSlot, _stateDoc);
                           auto updateResult = Helpers::upsert(opCtx,
                                                               _stateDocumentsNS.ns(),
                                                               filter,
                                                               getUpdatedStateDocBson(),
                                                               /*fromMigrate=*/false);

                           // We only want to insert, not modify, document
                           invariant(updateResult.numDocsModified == 0);
                           wuow.commit();
                       } else {
                           auto updateResult = Helpers::upsert(opCtx,
                                                               _stateDocumentsNS.ns(),
                                                               filter,
                                                               getUpdatedStateDocBson(),
                                                               /*fromMigrate=*/false);

                           // We only want to insert, not modify, document
                           invariant(updateResult.numDocsModified == 0);
                       }
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
        });
}

ExecutorFuture<repl::OpTime> ShardSplitDonorService::DonorStateMachine::_updateStateDocument(
    const ScopedTaskExecutorPtr& executor,
    const CancellationToken& token,
    ShardSplitDonorStateEnum nextState) {
    {
        stdx::lock_guard<Latch> lg(_mutex);
        LOGV2(6086505,
              "Updating state document {id} {state}",
              "id"_attr = _migrationId,
              "state"_attr = _stateDoc.getState());
    }

    auto tenantIds = [&]() {
        stdx::lock_guard<Latch> lg(_mutex);
        _stateDoc.setState(nextState);

        return _stateDoc.getTenantIds();
    }();

    return AsyncTry([this, tenantIds = std::move(tenantIds), uuid = _migrationId, nextState] {
               auto opCtxHolder = cc().makeOperationContext();
               auto opCtx = opCtxHolder.get();

               AutoGetCollection collection(opCtx, _stateDocumentsNS, MODE_IX);
               uassert(ErrorCodes::NamespaceNotFound,
                       str::stream() << _stateDocumentsNS.ns() << " does not exist",
                       collection);

               writeConflictRetry(
                   opCtx, "ShardSplitDonorUpdateStateDoc", _stateDocumentsNS.ns(), [&] {
                       WriteUnitOfWork wuow(opCtx);

                       if (nextState == ShardSplitDonorStateEnum::kBlocking) {
                           invariant(tenantIds);
                           setMtabToBlockingForTenants(_serviceContext, opCtx, tenantIds.get());
                       }
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

void ShardSplitDonorService::DonorStateMachine::_createReplicaSetMonitor(
    const ExecutorPtr& executor, const CancellationToken& abortToken) {

    auto connectionString = [&]() {
        stdx::lock_guard<Latch> lg(_mutex);

        return _stateDoc.getRecipientConnectionString();
    }();


    invariant(connectionString);

    _recipientAcceptedSplit.setFrom(
        getRecipientAcceptSplitFuture(
            executor, abortToken, MongoURI::parse(connectionString.get()).getValue())
            .unsafeToInlineFuture());
    _replicaSetMonitorCreatedPromise.emplaceValue();

    LOGV2(6142508,
          "Monitoring recipient nodes for split acceptance.",
          "recipientConnectionString"_attr = connectionString.get());
}

ExecutorFuture<ShardSplitDonorService::DonorStateMachine::DurableState>
ShardSplitDonorService::DonorStateMachine::_handleErrorOrEnterAbortedState(
    StatusWith<DurableState> statusWithState,
    const ScopedTaskExecutorPtr& executor,
    const CancellationToken& primaryToken,
    const CancellationToken& abortToken) {
    LOGV2(6086510,
          "Shard split error handling {id} {status}",
          "id"_attr = _migrationId,
          "status"_attr = statusWithState.getStatus());

    {
        stdx::lock_guard<Latch> lg(_mutex);
        if (isAbortedDocumentPersistent(lg, _stateDoc)) {
            // The document is already in aborted state. No need to write it.
            return ExecutorFuture(**executor,
                                  DurableState{ShardSplitDonorStateEnum::kAborted, _abortReason});
        }
    }

    if (primaryToken.isCanceled()) {
        return ExecutorFuture(
            **executor,
            StatusWith<DurableState>(ErrorCodes::InterruptedDueToReplStateChange,
                                     "Interrupted due to replica set state change"));
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

        LOGV2(6086508, "Shard split aborted {id}", "id"_attr = _migrationId);

        _abortReason = statusWithState.getStatus();
        BSONObjBuilder bob;
        _abortReason->serializeErrorToBSON(&bob);
        _stateDoc.setAbortReason(bob.obj());
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
}  // namespace mongo
