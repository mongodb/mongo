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


#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_coordinator_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/global_catalog/sharding_catalog_client.h"
#include "mongo/db/local_catalog/lock_manager/locker.h"
#include "mongo/db/local_catalog/shard_role_catalog/operation_sharding_state.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/write_concern.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/rpc/unique_message.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_util.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <algorithm>
#include <type_traits>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangBeforeRunningCoordinatorInstance);
MONGO_FAIL_POINT_DEFINE(hangBeforeRemovingCoordinatorDocument);

namespace {

const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());

}  // namespace

template <typename T>
ExecutorFuture<void> ShardingDDLCoordinator::_acquireLockAsync(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token,
    const T& resource,
    LockMode lockMode) {
    return AsyncTry([this, resource, lockMode] {
               auto opCtxHolder = makeOperationContext();
               auto* opCtx = opCtxHolder.get();

               const auto coorName = DDLCoordinatorType_serializer(_coordId.getOperationType());

               _scopedLocks.emplace(DDLLockManager::ScopedBaseDDLLock{opCtx,
                                                                      _locker.get(),
                                                                      resource,
                                                                      coorName,
                                                                      lockMode,
                                                                      false /* waitForRecovery */});
           })
        .until([this, resource, lockMode](Status status) {
            if (!status.isOK()) {
                LOGV2_WARNING(
                    6819300,
                    "DDL lock acquisition attempt failed",
                    logv2::DynamicAttributes{getCoordinatorLogAttrs(),
                                             "resource"_attr = toStringForLogging(resource),
                                             "mode"_attr = modeName(lockMode),
                                             "error"_attr = redact(status)});
            }
            // Sharding DDL operations are not rollbackable so in case we recovered a coordinator
            // from disk we need to ensure eventual completion of the DDL operation, so we must
            // retry until we manage to acquire the lock.
            return (!_recoveredFromDisk) || status.isOK();
        })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, token);
}

ShardingDDLCoordinatorMetadata extractShardingDDLCoordinatorMetadata(const BSONObj& coorDoc) {
    return ShardingDDLCoordinatorMetadata::parse(
        coorDoc, IDLParserContext("ShardingDDLCoordinatorMetadata"));
}

ShardingDDLCoordinator::ShardingDDLCoordinator(ShardingDDLCoordinatorService* service,
                                               const BSONObj& coorDoc)
    : _service(service),
      _coordId(extractShardingDDLCoordinatorMetadata(coorDoc).getId()),
      _recoveredFromDisk(extractShardingDDLCoordinatorMetadata(coorDoc).getRecoveredFromDisk()),
      _forwardableOpMetadata(
          extractShardingDDLCoordinatorMetadata(coorDoc).getForwardableOpMetadata()),
      _databaseVersion(extractShardingDDLCoordinatorMetadata(coorDoc).getDatabaseVersion()),
      _firstExecution(!_recoveredFromDisk),
      _externalState(_service->createExternalState()) {}

ShardingDDLCoordinator::~ShardingDDLCoordinator() {
    tassert(10644519,
            "Expected _constructionCompletionPromise to be ready",
            _constructionCompletionPromise.getFuture().isReady());
    tassert(10644520,
            "Expected _completionPromise to be ready",
            _completionPromise.getFuture().isReady());
}

ExecutorFuture<bool> ShardingDDLCoordinator::_removeDocumentUntillSuccessOrStepdown(
    std::shared_ptr<executor::TaskExecutor> executor) {
    return AsyncTry([this, anchor = shared_from_this()] {
               auto opCtxHolder = makeOperationContext();
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
    _getExternalState()->waitForVectorClockDurable(opCtx);

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

        return deleteOp.serialize();
    }());

    const auto commandReply = commandResponse->getCommandReply();
    uassertStatusOK(getStatusFromWriteCommandReply(commandReply));

    BatchedCommandResponse batchedResponse;
    std::string unusedErrmsg;
    batchedResponse.parseBSON(commandReply, &unusedErrmsg);

    WriteConcernResult ignoreResult;
    const WriteConcernOptions majorityWriteConcern{WriteConcernOptions::kMajority,
                                                   WriteConcernOptions::SyncMode::UNSET,
                                                   WriteConcernOptions::kNoTimeout};
    auto latestOpTime = repl::ReplClientInfo::forClient(opCtx->getClient()).getLastOp();
    uassertStatusOK(waitForWriteConcern(opCtx, latestOpTime, majorityWriteConcern, &ignoreResult));

    return batchedResponse.getN() > 0;
}


ExecutorFuture<void> ShardingDDLCoordinator::_translateTimeseriesNss(
    std::shared_ptr<executor::ScopedTaskExecutor> executor, const CancellationToken& token) {

    return AsyncTry([this] {
               auto opCtxHolder = makeOperationContext();
               auto* opCtx = opCtxHolder.get();

               const auto bucketNss = originalNss().makeTimeseriesBucketsNamespace();

               if (_getExternalState()->isTrackedTimeseries(opCtx, bucketNss)) {
                   auto coordMetadata = metadata();
                   coordMetadata.setBucketNss(bucketNss);
                   setMetadata(std::move(coordMetadata));
               }
           })
        .until([this](Status status) {
            if (!status.isOK()) {
                LOGV2_WARNING(6675600,
                              "Failed to fetch information for the bucket namespace",
                              logv2::DynamicAttributes{
                                  getCoordinatorLogAttrs(),
                                  logAttrs(originalNss().makeTimeseriesBucketsNamespace()),
                                  "error"_attr = redact(status)});
            }
            // Sharding DDL operations are not rollbackable so in case we recovered a coordinator
            // from disk we need to ensure eventual completion of the operation, so we must
            // retry keep retrying until success.
            return (!_recoveredFromDisk) || status.isOK();
        })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, token);
}

ExecutorFuture<void> ShardingDDLCoordinator::_acquireAllLocksAsync(
    OperationContext* opCtx,
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) {
    // Fetching all the locks that need to be acquired. Sort them by nss to avoid deadlocks.
    // If the requested nss represents a timeseries buckets namespace, translate it to its view nss.
    std::set<NamespaceString> locksToAcquire;
    locksToAcquire.insert(originalNss().isTimeseriesBucketsCollection()
                              ? originalNss().getTimeseriesViewNamespace()
                              : originalNss());

    for (const auto& additionalLock : _getAdditionalLocksToAcquire(opCtx)) {
        locksToAcquire.insert(additionalLock.isTimeseriesBucketsCollection()
                                  ? additionalLock.getTimeseriesViewNamespace()
                                  : additionalLock);
    }

    // Acquiring all DDL locks in sorted order to avoid deadlocks
    // Note that the sorted order is provided by default through the std::set container
    auto futureChain = ExecutorFuture<void>(**executor);
    boost::optional<DatabaseName> lastDb;
    for (const auto& lockNss : locksToAcquire) {
        const bool isDbOnly = lockNss.coll().empty();

        // Acquiring the database DDL lock
        const auto normalizedDbName = [&] {
            if (_coordId.getOperationType() != DDLCoordinatorTypeEnum::kCreateDatabase) {
                // Already existing databases are not allowed to have their names differ just on
                // case. Uses the requested database name directly.
                return lockNss.dbName();
            }
            const auto dbNameStr =
                DatabaseNameUtil::serialize(lockNss.dbName(), SerializationContext::stateDefault());
            return DatabaseNameUtil::deserialize(
                boost::none, str::toLower(dbNameStr), SerializationContext::stateDefault());
        }();
        if (lastDb != normalizedDbName) {
            const auto lockMode = (isDbOnly ? MODE_X : MODE_IX);
            futureChain = std::move(futureChain)
                              .then([this,
                                     executor,
                                     token,
                                     normalizedDbName,
                                     lockMode,
                                     anchor = shared_from_this()] {
                                  return _acquireLockAsync<DatabaseName>(
                                      executor, token, normalizedDbName, lockMode);
                              });
            lastDb = normalizedDbName;
        }

        // Acquiring the collection DDL lock
        if (!isDbOnly) {
            futureChain =
                std::move(futureChain)
                    .then([this, executor, token, nss = lockNss, anchor = shared_from_this()] {
                        return _acquireLockAsync<NamespaceString>(executor, token, nss, MODE_X);
                    });
        }
    }
    return futureChain;
}


ExecutorFuture<void> ShardingDDLCoordinator::_cleanupOnAbort(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token,
    const Status& status) noexcept {
    return ExecutorFuture<void>(**executor);
}

boost::optional<Status> ShardingDDLCoordinator::getAbortReason() const {
    return boost::none;
}

void ShardingDDLCoordinator::interrupt(Status status) {
    LOGV2_DEBUG(5390535,
                1,
                "Sharding DDL Coordinator received an interrupt",
                logv2::DynamicAttributes{getCoordinatorLogAttrs(), "reason"_attr = redact(status)});

    // Resolve any unresolved promises to avoid hanging.
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    if (!_constructionCompletionPromise.getFuture().isReady()) {
        _constructionCompletionPromise.setError(status);
    }
    if (!_completionPromise.getFuture().isReady()) {
        _completionPromise.setError(status);
    }
}

logv2::DynamicAttributes ShardingDDLCoordinator::getBasicCoordinatorAttrs() const {
    logv2::DynamicAttributes attrs;
    attrs.add("coordinatorId", _coordId);
    return attrs;
}

SemiFuture<void> ShardingDDLCoordinator::run(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                             const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, executor, token, anchor = shared_from_this()] {
            auto opCtxHolder = makeOperationContext();
            auto* opCtx = opCtxHolder.get();

            tassert(10644521, "Expected _locker to be unset", !_locker);
            _locker = std::make_unique<Locker>(opCtx->getServiceContext());
            _locker->unsetThreadId();
            _locker->setDebugInfo(str::stream() << _coordId.toBSON());

            // Check if this coordinator is allowed to start according to the user-writes blocking
            // critical section. If it is not the first execution, it means it had started already
            // and we are recovering this coordinator. In this case, let it be completed even though
            // new DDL operations may be prohibited now.
            // Coordinators that do not affect user data are allowed to start even when user writes
            // are blocked.
            if (_firstExecution && !canAlwaysStartWhenUserWritesAreDisabled()) {
                _getExternalState()->checkShardedDDLAllowedToStart(opCtx, originalNss());
            }
        })
        .then([this, executor, token, anchor = shared_from_this()] {
            auto opCtxHolder = makeOperationContext();
            auto* opCtx = opCtxHolder.get();

            return _acquireAllLocksAsync(opCtx, executor, token);
        })
        .then([this, executor, token, anchor = shared_from_this()] {
            if (!originalNss().isConfigDB() && !originalNss().isAdminDB() && !_recoveredFromDisk &&
                operationType() != DDLCoordinatorTypeEnum::kCreateDatabase) {
                auto opCtxHolder = makeOperationContext();
                auto* opCtx = opCtxHolder.get();

                tassert(10644522,
                        "Expected databaseVersion to be set on the coordinator document metadata",
                        metadata().getDatabaseVersion());

                ScopedSetShardRole scopedSetShardRole(
                    opCtx,
                    originalNss(),
                    boost::none /* shardVersion */,
                    metadata().getDatabaseVersion() /* databaseVersion */);

                _getExternalState()->assertIsPrimaryShardForDb(opCtx, originalNss().dbName());
            };
        })
        .then([this, executor, token, anchor = shared_from_this()] {
            if (!_firstExecution ||
                // this DDL operation operates on a DB
                originalNss().coll().empty() ||
                // this DDL operation operates directly on a bucket nss
                originalNss().isTimeseriesBucketsCollection()) {
                return ExecutorFuture<void>(**executor);
            }
            return _translateTimeseriesNss(executor, token);
        })
        .then([this, anchor = shared_from_this()] {
            stdx::lock_guard<stdx::mutex> lg(_mutex);
            if (!_constructionCompletionPromise.getFuture().isReady()) {
                _constructionCompletionPromise.emplaceValue();
            }

            hangBeforeRunningCoordinatorInstance.pauseWhileSet();
        })
        .onError([this, token, anchor = shared_from_this()](const Status& status) {
            // The construction of a DDL coordinator recovered from disk can only fail due to
            // stepdown/shutdown.
            tassert(10644523,
                    "Expected recovered sharding DDL coordinator to be reconstructed successfully",
                    !_recoveredFromDisk ||
                        (token.isCanceled() &&
                         (status.isA<ErrorCategory::CancellationError>() ||
                          status.isA<ErrorCategory::NotPrimaryError>())));

            // Ensure coordinator cleanup if the document has not been saved.
            _completeOnError = !_recoveredFromDisk;

            static constexpr auto& errorMsg =
                "Failed to complete construction of sharding DDL coordinator";
            LOGV2_ERROR(
                5390530,
                errorMsg,
                logv2::DynamicAttributes{getCoordinatorLogAttrs(), "reason"_attr = redact(status)});

            stdx::lock_guard<stdx::mutex> lg(_mutex);
            if (!_constructionCompletionPromise.getFuture().isReady()) {
                _constructionCompletionPromise.setError(status);
            }

            return status;
        })
        .then([this, executor, token, anchor = shared_from_this()] {
            return AsyncTry([this, executor, token] {
                       if (const auto& status = getAbortReason()) {
                           return _cleanupOnAbort(executor, token, *status);
                       }

                       return _runImpl(executor, token);
                   })
                .until([this, token](Status status) {
                    const bool shouldRetry = [&]() {
                        if (status.isOK() || token.isCanceled()) {
                            // Do not retry on success or PrimaryOnlyService interruption.
                            return false;
                        }

                        // From this point on status cannot be OK, and the token is not canceled.
                        // Proceed to check if the conditions for retrying apply.
                        if (_completeOnError) {
                            // The coordinator instance was marked to not retry on error.
                            return false;
                        }

                        if (_isRetriableErrorForDDLCoordinator(status)) {
                            // The error is considered retriable. The 'status' can be a result of an
                            // error which ocurred locally, or one which ocurred in a remote node.
                            // Due to this, some errors cannot be unambiguosly classified as
                            // retriable or non-retriable. We default to treating them as retriable
                            // and defer to the PrimaryOnlyService infrastructure, which cancels
                            // execution through the "token" when appropriate. This can result in a
                            // retry being logged, when in fact the coordinator is going to be
                            // interrupted immediately after.
                            return true;
                        }

                        if (_mustAlwaysMakeProgress()) {
                            // The coordinator implementation specifically requests to always retry.
                            return true;
                        }

                        if (getAbortReason()) {
                            // An abort reason is provided, meaning _cleanupOnAbort should be
                            // executed during the retry.
                            return true;
                        }

                        // None of the conditions for retrying are met.
                        return false;
                    }();

                    if (shouldRetry) {
                        _firstExecution = false;
                        LOGV2_INFO(5656000,
                                   "Re-executing sharding DDL coordinator",
                                   logv2::DynamicAttributes{getCoordinatorLogAttrs(),
                                                            "reason"_attr = redact(status)});
                    }
                    // Returning 'false' signals we need to retry.
                    return !shouldRetry;
                })
                .withBackoffBetweenIterations(kExponentialBackoff)
                .on(**executor, CancellationToken::uncancelable());
        })
        .onCompletion([this, executor, token, anchor = shared_from_this()](const Status& status) {
            auto opCtxHolder = makeOperationContext();
            auto* opCtx = opCtxHolder.get();

            // If we are stepping down the token MUST be cancelled. Each implementation of the
            // coordinator must retry remote stepping down errors, unless, we allow finalizing the
            // coordinator in the presence of errors.
            tassert(10644524,
                    "Expected the coordinator token to be cancelled on stepdown",
                    !(status.isA<ErrorCategory::NotPrimaryError>() ||
                      status.isA<ErrorCategory::ShutdownError>()) ||
                        token.isCanceled() || _completeOnError);

            auto completionStatus =
                !status.isOK() ? status : getAbortReason().get_value_or(Status::OK());

            bool isSteppingDown = token.isCanceled();

            // Remove the ddl coordinator and release locks if the execution was successfull or if
            // there was any error and we have the _completeOnError flag set or if we are not
            // stepping down.
            auto cleanup = [&]() {
                return completionStatus.isOK() || _completeOnError || !isSteppingDown;
            };

            if (cleanup()) {
                try {
                    if (!completionStatus.isOK()) {
                        LOGV2_ERROR(
                            7524000,
                            "Failed sharding DDL coordinator",
                            logv2::DynamicAttributes{getCoordinatorLogAttrs(),
                                                     "reason"_attr = redact(completionStatus)});
                    }

                    hangBeforeRemovingCoordinatorDocument.pauseWhileSet();

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
                    tassert(
                        10644525,
                        "Sharding DDL coordinator cleanup failed for a reason other than stepdown",
                        isSteppingDown);
                }
            }

            // Release all DDL locks
            while (!_scopedLocks.empty()) {
                _scopedLocks.pop();
            }

            stdx::lock_guard<stdx::mutex> lg(_mutex);
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
        if (std::find(participants.begin(), participants.end(), ShardId::kConfigServerId) ==
            participants.end()) {
            // The config server may be a shard, so only add if it isn't already in participants.
            participants.emplace_back(shardRegistry->getConfigShard()->getId());
        }
        return participants;
    }();

    sharding_ddl_util::performNoopRetryableWriteOnShards(opCtx, shardsAndConfigsvr, osi, executor);
}

bool ShardingDDLCoordinator::_isRetriableErrorForDDLCoordinator(const Status& status) {
    return status.isA<ErrorCategory::CursorInvalidatedError>() ||
        status.isA<ErrorCategory::ShutdownError>() || status.isA<ErrorCategory::RetriableError>() ||
        status.isA<ErrorCategory::Interruption>() ||
        status.isA<ErrorCategory::CancellationError>() ||
        status.isA<ErrorCategory::ExceededTimeLimitError>() ||
        status.isA<ErrorCategory::WriteConcernError>() ||
        status == ErrorCodes::FailedToSatisfyReadPreference || status == ErrorCodes::Interrupted ||
        status == ErrorCodes::LockBusy || status == ErrorCodes::CommandNotFound;
}

ShardingDDLCoordinatorExternalState* ShardingDDLCoordinator::_getExternalState() {
    return _externalState.get();
}

}  // namespace mongo
