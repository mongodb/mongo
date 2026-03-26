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


#include "mongo/db/global_catalog/ddl/sharding_coordinator.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/database_name.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/global_catalog/ddl/sharding_coordinator_gen.h"
#include "mongo/db/global_catalog/ddl/sharding_ddl_util.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/query/write_ops/write_ops_gen.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/wait_for_majority_service.h"
#include "mongo/db/shard_role/lock_manager/locker.h"
#include "mongo/db/shard_role/shard_catalog/operation_sharding_state.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/write_concern.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/log.h"
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

ShardingCoordinatorMetadata extractShardingCoordinatorMetadata(const BSONObj& coorDoc) {
    return ShardingCoordinatorMetadata::parse(coorDoc,
                                              IDLParserContext("ShardingCoordinatorMetadata"));
}

// Enables propagation of the versionContext (OFCV) to sub-operations of this ShardingCoordinator
// for network calls. For ShardingCoordinator, this is safe since their metadata is persisted to
// disk, which allows setFCV to drain them before it proceeding to the metadata cleanup phase.
ForwardableOperationMetadata enableVersionContextPropagation(
    ForwardableOperationMetadata forwardableOperationMetadata) {
    // TODO SERVER-99655: update once gSnapshotFCVInDDLCoordinators is enabled on lastLTS
    if (const auto& vCtx = forwardableOperationMetadata.getVersionContext()) {
        forwardableOperationMetadata.setVersionContext(vCtx->withPropagationAcrossShards_UNSAFE());
    }

    return forwardableOperationMetadata;
}

ShardingCoordinator::ShardingCoordinator(ShardingCoordinatorService* service,
                                         std::string name,
                                         const BSONObj& coorDoc)
    : _coordinatorName(std::move(name)),
      _service(service),
      _coordId(extractShardingCoordinatorMetadata(coorDoc).getId()),
      _recoveredFromDisk(extractShardingCoordinatorMetadata(coorDoc).getRecoveredFromDisk()),
      _forwardableOpMetadata(
          extractShardingCoordinatorMetadata(coorDoc).getForwardableOpMetadata().map(
              enableVersionContextPropagation)),
      _databaseVersion(extractShardingCoordinatorMetadata(coorDoc).getDatabaseVersion()),
      _firstExecution(!_recoveredFromDisk),
      _externalState(_service->createExternalState()) {}

ShardingCoordinator::~ShardingCoordinator() {
    tassert(10644519,
            "Expected _constructionCompletionPromise to be ready",
            _constructionCompletionPromise.getFuture().isReady());
    tassert(10644520,
            "Expected _completionPromise to be ready",
            _completionPromise.getFuture().isReady());
}

ExecutorFuture<bool> ShardingCoordinator::_removeDocumentUntillSuccessOrStepdown(
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

bool ShardingCoordinator::_removeDocument(OperationContext* opCtx) {
    // Checkpoint configTime and topologyTime to guarantee causality with respect to DDL operations
    _getExternalState()->waitForVectorClockDurable(opCtx);

    DBDirectClient dbClient(opCtx);
    auto commandResponse = dbClient.runCommand([&] {
        write_ops::DeleteCommandRequest deleteOp(
            NamespaceString::kShardingDDLCoordinatorsNamespace);

        deleteOp.setDeletes({[&] {
            write_ops::DeleteOpEntry entry;
            entry.setQ(BSON(ShardingCoordinatorMetadata::kIdFieldName << _coordId.toBSON()));
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


ExecutorFuture<void> ShardingCoordinator::_translateTimeseriesNss(
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
            // Coordinators can't generally be rolled back so in case we recovered a coordinator
            // from disk we need to ensure eventual completion of the operation, so we must keep
            // retrying until success.
            return (!_recoveredFromDisk) || status.isOK();
        })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(**executor, token);
}

ExecutorFuture<void> ShardingCoordinator::_cleanupOnAbort(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token,
    const Status& status) noexcept {
    return ExecutorFuture<void>(**executor);
}

boost::optional<Status> ShardingCoordinator::getAbortReason() const {
    return boost::none;
}

void ShardingCoordinator::interrupt(Status status) {
    LOGV2_DEBUG(5390535,
                1,
                "Sharding Coordinator received an interrupt",
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

logv2::DynamicAttributes ShardingCoordinator::getBasicCoordinatorAttrs() const {
    logv2::DynamicAttributes attrs;
    attrs.add("coordinatorId", _coordId);
    return attrs;
}

SemiFuture<void> ShardingCoordinator::run(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                          const CancellationToken& token) noexcept {
    return ExecutorFuture<void>(**executor)
        .then([this, executor, token, anchor = shared_from_this()] {
            auto opCtxHolder = makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            _initialize(opCtx);
        })
        .then([this, executor, token, anchor = shared_from_this()] {
            auto opCtxHolder = makeOperationContext();
            auto* opCtx = opCtxHolder.get();
            return _acquireLocksAsync(opCtx, executor, token);
        })
        .then([this, executor, token, anchor = shared_from_this()] {
            if (!originalNss().isConfigDB() && !originalNss().isAdminDB() && !_recoveredFromDisk &&
                operationType() != CoordinatorTypeEnum::kCreateDatabase) {
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
            // Coordinators that can't be aborted can only fail due to stepdown/shutdown.
            tassert(10644523,
                    "Expected recovered coordinator to be reconstructed successfully",
                    !_recoveredFromDisk ||
                        (token.isCanceled() &&
                         (status.isA<ErrorCategory::CancellationError>() ||
                          status.isA<ErrorCategory::NotPrimaryError>())));

            // Ensure cleanup if the coordinator wasn't recovered from disk. Otherwise, it will be
            // recovered again on step up, so don't clean up.
            _completeOnError = !_recoveredFromDisk;

            static constexpr auto& errorMsg =
                "Failed to complete construction of sharding coordinator";
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
                                   "Re-executing sharding coordinator",
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

            // Remove the coordinator and release locks if the execution was successful or if
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
                            "Failed sharding coordinator",
                            logv2::DynamicAttributes{getCoordinatorLogAttrs(),
                                                     "reason"_attr = redact(completionStatus)});
                    }

                    hangBeforeRemovingCoordinatorDocument.pauseWhileSet();

                    LOGV2(
                        5565601, "Releasing sharding coordinator", "coordinatorId"_attr = _coordId);

                    // We need to execute this in another executor to ensure the remove work is
                    // done.
                    const auto docWasRemoved = _removeDocumentUntillSuccessOrStepdown(
                                                   _service->getInstanceCleanupExecutor())
                                                   .get();

                    if (!docWasRemoved) {
                        // Release the instance without interrupting it.
                        _service->releaseInstance(
                            BSON(ShardingCoordinatorMetadata::kIdFieldName << _coordId.toBSON()),
                            Status::OK());
                    }

                    if (status.isOK()) {
                        _onCleanup(opCtx);
                    }
                } catch (const DBException& ex) {
                    completionStatus = ex.toStatus();
                    // Ensure the only possible error is that we're stepping down.
                    isSteppingDown = completionStatus.isA<ErrorCategory::NotPrimaryError>() ||
                        completionStatus.isA<ErrorCategory::ShutdownError>() ||
                        completionStatus.isA<ErrorCategory::CancellationError>();
                    tassert(10644525,
                            "Sharding coordinator cleanup failed for a reason other than stepdown",
                            isSteppingDown);
                }
            }

            _releaseLocks(opCtx);

            stdx::lock_guard<stdx::mutex> lg(_mutex);
            if (!_completionPromise.getFuture().isReady()) {
                _completionPromise.setFrom(completionStatus);
            }

            return completionStatus;
        })
        .semi();
}

bool ShardingCoordinator::_isRetriableErrorForDDLCoordinator(const Status& status) {
    return status.isA<ErrorCategory::CursorInvalidatedError>() ||
        status.isA<ErrorCategory::ShutdownError>() || status.isA<ErrorCategory::RetriableError>() ||
        status.isA<ErrorCategory::Interruption>() ||
        status.isA<ErrorCategory::CancellationError>() ||
        status.isA<ErrorCategory::ExceededTimeLimitError>() ||
        status.isA<ErrorCategory::WriteConcernError>() ||
        status == ErrorCodes::FailedToSatisfyReadPreference || status == ErrorCodes::LockBusy ||
        status == ErrorCodes::CommandNotFound;
}

ShardingCoordinatorExternalState* ShardingCoordinator::_getExternalState() {
    return _externalState.get();
}

BSONObjBuilder ShardingCoordinator::basicReportBuilder() const noexcept {
    BSONObjBuilder bob;

    // Append static info
    bob.append("type", "op");
    bob.append("ns",
               NamespaceStringUtil::serialize(originalNss(), SerializationContext::stateDefault()));
    bob.append("desc", _coordinatorName);
    bob.append("op", "command");
    bob.append("active", true);

    // Append dynamic fields from the state doc
    {
        stdx::lock_guard lk{_docMutex};
        if (const auto& bucketNss = getDoc().getShardingCoordinatorMetadata().getBucketNss()) {
            // Bucket namespace is only present in case the collection is a sharded timeseries
            bob.append("bucketNamespace",
                       NamespaceStringUtil::serialize(bucketNss.get(),
                                                      SerializationContext::stateDefault()));
        }
    }

    // Create command description
    BSONObjBuilder cmdInfoBuilder;
    {
        stdx::lock_guard lk{_docMutex};
        if (const auto& optComment = getForwardableOpMetadata().getComment()) {
            cmdInfoBuilder.append(optComment.get().firstElement());
        }
    }
    appendCommandInfo(&cmdInfoBuilder);
    bob.append("command", cmdInfoBuilder.obj());

    return bob;
}

std::function<void()> RecoverableShardingCoordinator::_buildPhaseHandlerGeneric(
    CoordinatorGenericPhase newPhase, std::function<void(OperationContext*)>&& handlerFn) {
    return _buildPhaseHandlerGeneric(
        newPhase, [](OperationContext*) { return true; }, std::move(handlerFn));
}

std::function<void()> RecoverableShardingCoordinator::_buildPhaseHandlerGeneric(
    CoordinatorGenericPhase newPhase,
    std::function<bool(OperationContext*)>&& shouldExecute,
    std::function<void(OperationContext*)>&& handlerFn) {
    return [=, this] {
        const auto currPhase = [this] {
            stdx::lock_guard lk{_docMutex};
            return getDoc().getGenericPhase();
        }();

        if (currPhase > newPhase) {
            // Do not execute this phase if we already reached a subsequent one.
            return;
        }

        auto opCtxHolder = this->makeOperationContext();
        auto* opCtx = opCtxHolder.get();

        if (!shouldExecute(opCtx)) {
            // Do not execute the phase if the passed in condition is not met.
            return;
        }

        if (currPhase < newPhase) {
            // Persist the new phase if this is the first time we are executing it.
            _enterPhaseGeneric(newPhase);
        }

        return handlerFn(opCtx);
    };
}

void RecoverableShardingCoordinator::_enterPhaseGeneric(CoordinatorGenericPhase newPhase) {
    auto newDoc = _cloneDoc();
    const auto& currentDoc = getDoc();

    newDoc->setGenericPhase(newPhase);

    LOGV2_DEBUG(5390501,
                2,
                "sharding coordinator phase transition",
                "coordinatorId"_attr = currentDoc.getShardingCoordinatorMetadata().getId(),
                "newPhase"_attr = serializeGenericPhase(newDoc->getGenericPhase()),
                "oldPhase"_attr = serializeGenericPhase(currentDoc.getGenericPhase()));

    ServiceContext::UniqueOperationContext uniqueOpCtx;
    auto opCtx = cc().getOperationContext();
    if (!opCtx) {
        uniqueOpCtx = this->makeOperationContext();
        opCtx = uniqueOpCtx.get();
    }

    if (currentDoc.getGenericPhase() == CoordinatorGenericPhase::kUnset) {
        _insertStateDocumentGeneric(opCtx, std::move(newDoc));
    } else {
        _updateStateDocumentGeneric(opCtx, std::move(newDoc));
    }
}

BSONObjBuilder RecoverableShardingCoordinator::basicReportBuilder() const noexcept {
    auto baseReportBuilder = ShardingCoordinator::basicReportBuilder();

    const auto currPhase = [this] {
        stdx::lock_guard l{_docMutex};
        return getDoc().getGenericPhase();
    }();

    baseReportBuilder.append("currentPhase", serializeGenericPhase(currPhase));
    return baseReportBuilder;
}

void RecoverableShardingCoordinator::_insertStateDocumentGeneric(
    OperationContext* opCtx, std::unique_ptr<CoordinatorStateDoc> newDoc) {
    auto copyMetadata = newDoc->getShardingCoordinatorMetadata();
    copyMetadata.setRecoveredFromDisk(true);
    newDoc->setShardingCoordinatorMetadata(copyMetadata);

    PersistentTaskStore<CoordinatorStateDoc> store(
        NamespaceString::kShardingDDLCoordinatorsNamespace);
    try {
        store.add(opCtx, *newDoc, defaultMajorityWriteConcern());
    } catch (const ExceptionFor<ErrorCodes::DuplicateKey>&) {
        // A series of step-up and step-down events can cause a node to try and insert the
        // document when it has already been persisted locally, but we must still wait for
        // majority commit.
        const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        const auto lastLocalOpTime = replCoord->getMyLastAppliedOpTime();
        WaitForMajorityService::get(opCtx->getServiceContext())
            .waitUntilMajorityForWrite(lastLocalOpTime, opCtx->getCancellationToken())
            .get(opCtx);
    }

    {
        stdx::lock_guard lk{_docMutex};
        getDoc().replace(std::move(newDoc));
    }
}

void RecoverableShardingCoordinator::_updateStateDocumentGeneric(
    OperationContext* opCtx, std::unique_ptr<CoordinatorStateDoc> newDoc) {
    PersistentTaskStore<CoordinatorStateDoc> store(
        NamespaceString::kShardingDDLCoordinatorsNamespace);
    const auto& coorMetadata = newDoc->getShardingCoordinatorMetadata();
    tassert(10644540,
            "Expected recoveredFromDisk to be set on the coordinator document metadata",
            coorMetadata.getRecoveredFromDisk());
    store.update(opCtx,
                 BSON(CoordinatorStateDoc::kIdFieldName << coorMetadata.getId().toBSON()),
                 newDoc->toBSON(),
                 defaultMajorityWriteConcern());

    {
        stdx::lock_guard lk{_docMutex};
        getDoc().replace(std::move(newDoc));
    }
}

boost::optional<Status> RecoverableShardingCoordinator::getAbortReason() const {
    const auto& status = getDoc().getShardingCoordinatorMetadata().getAbortReason();
    tassert(10644541, "when persisted, status must be an error", !status || !status->isOK());
    return status;
}

void RecoverableShardingCoordinator::triggerCleanup(OperationContext* opCtx, const Status& status) {
    LOGV2_INFO(7418502,
               "Coordinator failed, persisting abort reason",
               "coordinatorId"_attr = getDoc().getShardingCoordinatorMetadata().getId(),
               "phase"_attr = serializeGenericPhase(getDoc().getGenericPhase()),
               "reason"_attr = redact(status));

    auto newDoc = _cloneDoc();

    auto coordinatorMetadata = newDoc->getShardingCoordinatorMetadata();

    coordinatorMetadata.setAbortReason(sharding_ddl_util::possiblyTruncateErrorStatus(status));
    newDoc->setShardingCoordinatorMetadata(std::move(coordinatorMetadata));

    _updateStateDocumentGeneric(opCtx, std::move(newDoc));

    uassertStatusOK(status);
}

void RecoverableShardingCoordinator::_onCleanup(OperationContext* opCtx) {
    releaseSession(opCtx);
}

boost::optional<OperationSessionInfo> RecoverableShardingCoordinator::readSession(
    OperationContext* opCtx) const {
    auto optSession = [&] {
        stdx::lock_guard lk{_docMutex};
        return getDoc().getShardingCoordinatorMetadata().getSession();
    }();
    if (!optSession) {
        return boost::none;
    }

    OperationSessionInfo osi;
    osi.setSessionId(optSession->getLsid());
    osi.setTxnNumber(optSession->getTxnNumber());
    return osi;
}

void RecoverableShardingCoordinator::writeSession(
    OperationContext* opCtx, const boost::optional<OperationSessionInfo>& osi) {
    if (!osi) {
        // The tracker will call writeSession with boost::none after calling releaseSession; by
        // the time the DDL coordinator does this, we've already deleted our state document.
        return;
    }
    auto newDoc = _cloneDoc();
    auto newMetadata = newDoc->getShardingCoordinatorMetadata();
    newMetadata.setSession(CoordinatorSession(*osi->getSessionId(), *osi->getTxnNumber()));
    newDoc->setShardingCoordinatorMetadata(std::move(newMetadata));
    _updateStateDocumentGeneric(opCtx, std::move(newDoc));
}

}  // namespace mongo
