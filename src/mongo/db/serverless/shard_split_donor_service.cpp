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
#include "mongo/db/catalog_raii.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/executor/cancelable_executor.h"
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

MONGO_FAIL_POINT_DEFINE(pauseShardSplitAfterInitialSync);

}  // namespace

ThreadPool::Limits ShardSplitDonorService::getThreadPoolLimits() const {
    return ThreadPool::Limits();
}

std::shared_ptr<repl::PrimaryOnlyService::Instance> ShardSplitDonorService::constructInstance(
    BSONObj initialState) {
    return std::make_shared<DonorStateMachine>(
        _serviceContext, this, ShardSplitDonorDocument::parse({"DonorStateMachine"}, initialState));
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
        if (_abortRequested) {
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
            .then([this, executor, cancelableExecutor, abortToken] {
                // Passing executor as a TaskExecutor is required by the downstream call to
                // AsyncTryUntilWithDelay::on(...)
                return _enterDataSyncState(executor, abortToken).thenRunOn(cancelableExecutor);
            })
            .then([this, cancelableExecutor] { pauseShardSplitAfterInitialSync.pauseWhileSet(); })
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
        if (_stateDoc.getState() != ShardSplitDonorStateEnum::kUninitialized) {
            return ExecutorFuture(**executor);
        }

        _stateDoc.setState(ShardSplitDonorStateEnum::kDataSync);
    }

    LOGV2(6086507, "Shard split entering data sync state {id}", "id"_attr = _migrationId);

    return _updateStateDocument(executor, abortToken)
        .then([this, executor, abortToken](repl::OpTime opTime) {
            return _waitForMajorityWriteConcern(executor, std::move(opTime), abortToken);
        });
}

ExecutorFuture<void> ShardSplitDonorService::DonorStateMachine::_writeInitialDocument(
    const ScopedTaskExecutorPtr& executor, const CancellationToken& primaryServiceToken) {

    {
        stdx::lock_guard<Latch> lg(_mutex);
        if (_stateDoc.getState() != ShardSplitDonorStateEnum::kUninitialized) {
            // If the state is not uninitialized, the document has already been written.
            return ExecutorFuture(**executor);
        }

        LOGV2(6086504,
              "Inserting initial state document {id} {state}",
              "id"_attr = _migrationId,
              "state"_attr = _stateDoc.getState());
    }

    return AsyncTry([this, uuid = _migrationId]() {
               auto opCtxHolder = cc().makeOperationContext();
               auto opCtx = opCtxHolder.get();

               AutoGetCollection collection(opCtx, _stateDocumentsNS, MODE_IX);

               writeConflictRetry(
                   opCtx, "ShardSplitDonorInsertStateDoc", _stateDocumentsNS.ns(), [&] {
                       const auto filter = BSON(ShardSplitDonorDocument::kIdFieldName << uuid);
                       const auto updateMod = [&]() {
                           stdx::lock_guard<Latch> lg(_mutex);
                           return BSON("$setOnInsert" << _stateDoc.toBSON());
                       }();
                       auto updateResult = Helpers::upsert(opCtx,
                                                           _stateDocumentsNS.ns(),
                                                           filter,
                                                           updateMod,
                                                           /*fromMigrate=*/false);

                       // We only want to insert, not modify, document
                       invariant(updateResult.numDocsModified == 0);
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
    const ScopedTaskExecutorPtr& executor, const CancellationToken& token) {
    {
        stdx::lock_guard<Latch> lg(_mutex);
        LOGV2(6086505,
              "Updating state document {id} {state}",
              "id"_attr = _migrationId,
              "state"_attr = _stateDoc.getState());
    }

    return AsyncTry([this, uuid = _migrationId] {
               auto opCtxHolder = cc().makeOperationContext();
               auto opCtx = opCtxHolder.get();

               AutoGetCollection collection(opCtx, _stateDocumentsNS, MODE_IX);

               writeConflictRetry(
                   opCtx, "ShardSplitDonorUpdateStateDoc", _stateDocumentsNS.ns(), [&] {
                       const auto filter = BSON(ShardSplitDonorDocument::kIdFieldName << uuid);
                       const auto updateMod = [&]() {
                           stdx::lock_guard<Latch> lg(_mutex);
                           return BSON("$set" << _stateDoc.toBSON());
                       }();
                       auto updateResult = Helpers::upsert(opCtx,
                                                           _stateDocumentsNS.ns(),
                                                           filter,
                                                           updateMod,
                                                           /*fromMigrate=*/false);

                       invariant(updateResult.numDocsModified == 1);
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

        if (_stateDoc.getState() == ShardSplitDonorStateEnum::kAborted) {
            // The shard split was resumed on stepup and it was already aborted
            return ExecutorFuture(**executor, statusWithState);
        }
    }

    if (primaryToken.isCanceled()) {
        return ExecutorFuture(
            **executor,
            StatusWith<DurableState>(ErrorCodes::InterruptedDueToReplStateChange,
                                     "Interrupted due to replicat set state change"));
    }

    // There is no use to check the parent token the executor would not run if the parent token
    // is canceled. At this point either the abortToken has been canceled or a previous operation
    // failed. In either case we abort the migration.
    if (abortToken.isCanceled()) {
        statusWithState =
            Status(ErrorCodes::TenantMigrationAborted, "Aborted due to abortShardSplit.");
    }

    {
        stdx::lock_guard<Latch> lg(_mutex);

        LOGV2(6086508, "Shard split aborted {id}", "id"_attr = _migrationId);

        _abortReason = statusWithState.getStatus();
        _stateDoc.setState(ShardSplitDonorStateEnum::kAborted);
    }

    return _updateStateDocument(executor, primaryToken)
        .then([this, executor, primaryToken](repl::OpTime opTime) {
            return _waitForMajorityWriteConcern(executor, std::move(opTime), primaryToken);
        })
        .then([this, executor] {
            stdx::lock_guard<Latch> lg(_mutex);

            return DurableState{_stateDoc.getState(), _abortReason};
        });
}
}  // namespace mongo
