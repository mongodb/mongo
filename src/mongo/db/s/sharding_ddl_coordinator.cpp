/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/sharding_ddl_coordinator.h"

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/logical_session_id_helpers.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_ddl_coordinator_gen.h"
#include "mongo/db/s/sharding_ddl_util.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/db/write_concern.h"
#include "mongo/logv2/log.h"
#include "mongo/s/grid.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/future_util.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangBeforeRunningCoordinatorInstance);
MONGO_FAIL_POINT_DEFINE(overrideDDLLockTimeout);

namespace {

const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());

}

ShardingDDLCoordinatorMetadata extractShardingDDLCoordinatorMetadata(const BSONObj& coorDoc) {
    return ShardingDDLCoordinatorMetadata::parse(
        IDLParserErrorContext("ShardingDDLCoordinatorMetadata"), coorDoc);
}

ShardingDDLCoordinator::ShardingDDLCoordinator(ShardingDDLCoordinatorService* service,
                                               const BSONObj& coorDoc)
    : _service(service),
      _coordId(extractShardingDDLCoordinatorMetadata(coorDoc).getId()),
      _recoveredFromDisk(extractShardingDDLCoordinatorMetadata(coorDoc).getRecoveredFromDisk()),
      _firstExecution(!_recoveredFromDisk) {}

ShardingDDLCoordinator::~ShardingDDLCoordinator() {
    invariant(_constructionCompletionPromise.getFuture().isReady());
    invariant(_completionPromise.getFuture().isReady());
}

ExecutorFuture<bool> ShardingDDLCoordinator::_removeDocumentUntillSuccessOrStepdown(
    std::shared_ptr<executor::TaskExecutor> executor) {
    return AsyncTry([this, anchor = shared_from_this()] {
               auto opCtxHolder = cc().makeOperationContext();
               auto* opCtx = opCtxHolder.get();

               return StatusWith(_removeDocument(opCtx));
           })
        .until([this](const StatusWith<bool>& sw) {
            // We can't rely on the instance token because after removing the document the
            // CancellationSource object of the instance is lost, so the reference to the parent POS
            // token is also lost, making any subsequent cancel during a stepdown unnoticeable by
            // the token.
            return sw.isOK() || sw.getStatus().isA<ErrorCategory::NotPrimaryError>() ||
                sw.getStatus().isA<ErrorCategory::ShutdownError>();
        })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(executor, CancellationToken::uncancelable());
}

bool ShardingDDLCoordinator::_removeDocument(OperationContext* opCtx) {
    // Checkpoint configTime and topologyTime to guarantee causality with respect to DDL operations
    VectorClockMutable::get(opCtx)->waitForDurable().get(opCtx);

    DBDirectClient dbClient(opCtx);
    auto commandResponse = dbClient.runCommand([&] {
        write_ops::DeleteCommandRequest deleteOp(
            NamespaceString::kShardingDDLCoordinatorsNamespace);

        deleteOp.setDeletes({[&] {
            write_ops::DeleteOpEntry entry;
            entry.setQ(BSON(ShardingDDLCoordinatorMetadata::kIdFieldName << _coordId.toBSON()));
            entry.setMulti(true);
            return entry;
        }()});

        return deleteOp.serialize({});
    }());

    const auto commandReply = commandResponse->getCommandReply();
    uassertStatusOK(getStatusFromWriteCommandReply(commandReply));

    BatchedCommandResponse batchedResponse;
    std::string unusedErrmsg;
    batchedResponse.parseBSON(commandReply, &unusedErrmsg);

    WriteConcernResult ignoreResult;
    const WriteConcernOptions majorityWriteConcern{
        WriteConcernOptions::kMajority,
        WriteConcernOptions::SyncMode::UNSET,
        WriteConcernOptions::kWriteConcernTimeoutSharding};
    auto latestOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    uassertStatusOK(waitForWriteConcern(opCtx, latestOpTime, majorityWriteConcern, &ignoreResult));

    return batchedResponse.getN() > 0;
}


ExecutorFuture<void> ShardingDDLCoordinator::_acquireLockAsync(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token,
    StringData resource) {
    return AsyncTry([this, resource = resource.toString()] {
               auto opCtxHolder = cc().makeOperationContext();
               auto* opCtx = opCtxHolder.get();
               auto distLockManager = DistLockManager::get(opCtx);

               const auto coorName = DDLCoordinatorType_serializer(_coordId.getOperationType());

               const auto lockTimeOut = [&]() -> Milliseconds {
                   if (auto sfp = overrideDDLLockTimeout.scoped(); MONGO_unlikely(sfp.isActive())) {
                       if (auto timeoutElem = sfp.getData()["timeoutMillisecs"]; timeoutElem.ok()) {
                           const auto timeoutMillisecs = Milliseconds(timeoutElem.safeNumberLong());
                           LOGV2(6320700,
                                 "Overriding DDL lock timeout",
                                 "timeout"_attr = timeoutMillisecs);
                           return timeoutMillisecs;
                       }
                   }
                   return DistLockManager::kDefaultLockTimeout;
               }();

               auto distLock = distLockManager->lockDirectLocally(opCtx, resource, lockTimeOut);
               _scopedLocks.emplace(std::move(distLock));

               uassertStatusOK(distLockManager->lockDirect(opCtx, resource, coorName, lockTimeOut));
           })
        .until([this](Status status) { return (!_recoveredFromDisk) || status.isOK(); })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, token);
}

void ShardingDDLCoordinator::interrupt(Status status) {
    LOGV2_DEBUG(5390535,
                1,
                "Sharding DDL Coordinator received an interrupt",
                "coordinatorId"_attr = _coordId,
                "reason"_attr = redact(status));

    // Resolve any unresolved promises to avoid hanging.
    stdx::lock_guard<Latch> lg(_mutex);
    if (!_constructionCompletionPromise.getFuture().isReady()) {
        _constructionCompletionPromise.setError(status);
    }
    if (!_completionPromise.getFuture().isReady()) {
        _completionPromise.setError(status);
    }
}

SemiFuture<void> ShardingDDLCoordinator::run(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                             const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, executor, token, anchor = shared_from_this()] {
            return _acquireLockAsync(executor, token, nss().db());
        })
        .then([this, executor, token, anchor = shared_from_this()] {
            if (!nss().isConfigDB() && !_recoveredFromDisk) {
                auto opCtxHolder = cc().makeOperationContext();
                auto* opCtx = opCtxHolder.get();
                invariant(metadata().getDatabaseVersion());

                OperationShardingState::get(opCtx).initializeClientRoutingVersions(
                    nss(), boost::none /* ChunkVersion */, metadata().getDatabaseVersion());
                // Check under the dbLock if this is still the primary shard for the database
                DatabaseShardingState::checkIsPrimaryShardForDb(opCtx, nss().db());
            };
        })
        .then([this, executor, token, anchor = shared_from_this()] {
            if (!nss().coll().empty()) {
                return _acquireLockAsync(executor, token, nss().ns());
            }
            return ExecutorFuture<void>(**executor);
        })
        .then([this, executor, token, anchor = shared_from_this()] {
            auto opCtxHolder = cc().makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            auto additionalLocks = _acquireAdditionalLocks(opCtx);
            if (!additionalLocks.empty()) {
                invariant(additionalLocks.size() == 1);
                return _acquireLockAsync(executor, token, additionalLocks.front());
            }
            return ExecutorFuture<void>(**executor);
        })
        .then([this, anchor = shared_from_this()] {
            stdx::lock_guard<Latch> lg(_mutex);
            if (!_constructionCompletionPromise.getFuture().isReady()) {
                _constructionCompletionPromise.emplaceValue();
            }

            hangBeforeRunningCoordinatorInstance.pauseWhileSet();
        })
        .onError([this, token, anchor = shared_from_this()](const Status& status) {
            // The construction of a DDL coordinator recovered from disk can only fail due to
            // stepdown/shutdown.
            dassert(!_recoveredFromDisk ||
                    (token.isCanceled() &&
                     (status.isA<ErrorCategory::CancellationError>() ||
                      status.isA<ErrorCategory::NotPrimaryError>())));

            // Ensure coordinator cleanup if the document has not been saved.
            _completeOnError = !_recoveredFromDisk;

            static constexpr auto& errorMsg =
                "Failed to complete construction of sharding DDL coordinator";
            LOGV2_ERROR(
                5390530, errorMsg, "coordinatorId"_attr = _coordId, "reason"_attr = redact(status));

            stdx::lock_guard<Latch> lg(_mutex);
            if (!_constructionCompletionPromise.getFuture().isReady()) {
                _constructionCompletionPromise.setError(status);
            }

            return status;
        })
        .then([this, executor, token, anchor = shared_from_this()] {
            return AsyncTry([this, executor, token] { return _runImpl(executor, token); })
                .until([this, token](Status status) {
                    // Retry until either:
                    //  - The coordinator succeed
                    //  - The coordinator failed with non-retryable error determined by the
                    //  coordinator, or an already known retryable error
                    //
                    //  If the token is not cancelled we retry because it could have been generated
                    //  by a remote node.
                    if (!status.isOK() && !_completeOnError &&
                        (_mustAlwaysMakeProgress() ||
                         status.isA<ErrorCategory::CursorInvalidatedError>() ||
                         status.isA<ErrorCategory::ShutdownError>() ||
                         status.isA<ErrorCategory::RetriableError>() ||
                         status.isA<ErrorCategory::CancellationError>() ||
                         status.isA<ErrorCategory::ExceededTimeLimitError>() ||
                         status.isA<ErrorCategory::WriteConcernError>() ||
                         status == ErrorCodes::FailedToSatisfyReadPreference ||
                         status == ErrorCodes::Interrupted || status == ErrorCodes::LockBusy ||
                         status == ErrorCodes::CommandNotFound) &&
                        !token.isCanceled()) {
                        LOGV2_INFO(5656000,
                                   "Re-executing sharding DDL coordinator",
                                   "coordinatorId"_attr = _coordId,
                                   "reason"_attr = redact(status));
                        _firstExecution = false;
                        return false;
                    }
                    return true;
                })
                .withBackoffBetweenIterations(kExponentialBackoff)
                .on(**executor, CancellationToken::uncancelable());
        })
        .onCompletion([this, executor, token, anchor = shared_from_this()](const Status& status) {
            auto opCtxHolder = cc().makeOperationContext();
            auto* opCtx = opCtxHolder.get();

            // If we are stepping down the token MUST be cancelled. Each implementation of the
            // coordinator must retry remote stepping down errors, unless, we allow finalizing the
            // coordinator in the presence of errors.
            dassert(!(status.isA<ErrorCategory::NotPrimaryError>() ||
                      status.isA<ErrorCategory::ShutdownError>()) ||
                    token.isCanceled() || _completeOnError);

            auto completionStatus = status;

            bool isSteppingDown = token.isCanceled();

            // Remove the ddl coordinator and release locks if the execution was successfull or if
            // there was any error and we have the _completeOnError flag set or if we are not
            // stepping down.
            auto cleanup = [&]() {
                return completionStatus.isOK() || _completeOnError || !isSteppingDown;
            };

            if (cleanup()) {
                try {
                    LOGV2(5565601,
                          "Releasing sharding DDL coordinator",
                          "coordinatorId"_attr = _coordId);

                    // We need to execute this in another executor to ensure the remove work is
                    // done.
                    const auto docWasRemoved = _removeDocumentUntillSuccessOrStepdown(
                                                   _service->getInstanceCleanupExecutor())
                                                   .get();

                    if (!docWasRemoved) {
                        // Release the instance without interrupting it.
                        _service->releaseInstance(
                            BSON(ShardingDDLCoordinatorMetadata::kIdFieldName << _coordId.toBSON()),
                            Status::OK());
                    }

                    auto session = metadata().getSession();

                    if (status.isOK() && session) {
                        // Return lsid to the SessionCache. If status is not OK, let the lsid be
                        // discarded.
                        InternalSessionPool::get(opCtx)->release(
                            {session->getLsid(), session->getTxnNumber()});
                    }
                } catch (const DBException& ex) {
                    completionStatus = ex.toStatus();
                    // Ensure the only possible error is that we're stepping down.
                    isSteppingDown = completionStatus.isA<ErrorCategory::NotPrimaryError>() ||
                        completionStatus.isA<ErrorCategory::ShutdownError>() ||
                        completionStatus.isA<ErrorCategory::CancellationError>();
                    dassert(isSteppingDown);
                }
            }

            if (!cleanup()) {
                LOGV2(5950000,
                      "Not releasing distributed locks because the node is stepping down or "
                      "shutting down",
                      "coordinatorId"_attr = _coordId,
                      "status"_attr = status);
            }

            while (!_scopedLocks.empty()) {
                if (cleanup()) {
                    // (SERVER-59500) Only release the remote locks in case of no stepdown/shutdown
                    const auto& resource = _scopedLocks.top().getNs();
                    DistLockManager::get(opCtx)->unlock(opCtx, resource);
                }
                _scopedLocks.pop();
            }

            stdx::lock_guard<Latch> lg(_mutex);
            if (!_completionPromise.getFuture().isReady()) {
                _completionPromise.setFrom(completionStatus);
            }

            return completionStatus;
        })
        .semi();
}

void ShardingDDLCoordinator::_performNoopRetryableWriteOnAllShardsAndConfigsvr(
    OperationContext* opCtx,
    const OperationSessionInfo& osi,
    const std::shared_ptr<executor::TaskExecutor>& executor) {
    const auto shardsAndConfigsvr = [&] {
        const auto shardRegistry = Grid::get(opCtx)->shardRegistry();
        auto participants = shardRegistry->getAllShardIds(opCtx);
        participants.emplace_back(shardRegistry->getConfigShard()->getId());
        return participants;
    }();

    sharding_ddl_util::performNoopRetryableWriteOnShards(opCtx, shardsAndConfigsvr, osi, executor);
}

}  // namespace mongo
