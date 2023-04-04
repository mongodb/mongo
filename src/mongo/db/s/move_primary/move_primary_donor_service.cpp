/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/s/move_primary/move_primary_donor_service.h"

#include "mongo/db/persistent_task_store.h"
#include "mongo/db/s/move_primary/move_primary_recipient_cmds_gen.h"
#include "mongo/db/s/move_primary/move_primary_server_parameters_gen.h"
#include "mongo/s/grid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kMovePrimary

namespace mongo {
namespace {
// Both of these failpoints have the same implementation. A single failpoint can't be active
// multiple times with different arguments, but setting up more complex scenarios sometimes requires
// multiple failpoints.
MONGO_FAIL_POINT_DEFINE(pauseDuringMovePrimaryDonorStateEnumTransition);
MONGO_FAIL_POINT_DEFINE(pauseDuringMovePrimaryDonorStateEnumTransitionAlternate);

MONGO_FAIL_POINT_DEFINE(pauseBeforeBeginningMovePrimaryDonorWorkflow);
MONGO_FAIL_POINT_DEFINE(pauseBeforeBeginningMovePrimaryDonorCleanup);

enum StateTransitionProgress {
    kBefore,   // Prior to any changes for state.
    kPartial,  // After updating on-disk state, but before updating in-memory state.
    kAfter     // After updating in-memory state.
};

const auto kProgressArgMap = [] {
    return stdx::unordered_map<std::string, StateTransitionProgress>{
        {"before", StateTransitionProgress::kBefore},
        {"partial", StateTransitionProgress::kPartial},
        {"after", StateTransitionProgress::kAfter}};
}();

boost::optional<StateTransitionProgress> readProgressArgument(const BSONObj& data) {
    auto arg = data.getStringField("progress");
    auto it = kProgressArgMap.find(arg.toString());
    if (it == kProgressArgMap.end()) {
        return boost::none;
    }
    return it->second;
}

boost::optional<MovePrimaryDonorStateEnum> readStateArgument(const BSONObj& data) {
    try {
        auto arg = data.getStringField("state");
        IDLParserContext ectx("pauseDuringMovePrimaryDonorStateEnumTransition::readStateArgument");
        return MovePrimaryDonorState_parse(ectx, arg);
    } catch (...) {
        return boost::none;
    }
}

void evaluatePauseDuringStateTransitionFailpoint(StateTransitionProgress progress,
                                                 MovePrimaryDonorStateEnum newState,
                                                 FailPoint& failpoint) {
    failpoint.executeIf(
        [&](const auto& data) { failpoint.pauseWhileSet(); },
        [&](const auto& data) {
            auto desiredProgress = readProgressArgument(data);
            auto desiredState = readStateArgument(data);
            if (!desiredProgress.has_value() || !desiredState.has_value()) {
                LOGV2(7306200,
                      "pauseDuringMovePrimaryDonorStateEnumTransition failpoint data must contain "
                      "progress and state arguments",
                      "failpoint"_attr = failpoint.getName(),
                      "data"_attr = data);
                return false;
            }
            return *desiredProgress == progress && *desiredState == newState;
        });
}

void evaluatePauseDuringStateTransitionFailpoints(StateTransitionProgress progress,
                                                  MovePrimaryDonorStateEnum newState) {
    const auto fps = {std::ref(pauseDuringMovePrimaryDonorStateEnumTransition),
                      std::ref(pauseDuringMovePrimaryDonorStateEnumTransitionAlternate)};
    for (auto& fp : fps) {
        evaluatePauseDuringStateTransitionFailpoint(progress, newState, fp);
    }
}

Status deserializeStatus(const BSONObj& bson) {
    auto code = ErrorCodes::Error(bson["code"].numberInt());
    auto reason = bson["errmsg"].String();
    return Status{code, reason};
}

}  // namespace

MovePrimaryDonorService::MovePrimaryDonorService(ServiceContext* serviceContext)
    : PrimaryOnlyService{serviceContext}, _serviceContext{serviceContext} {}

StringData MovePrimaryDonorService::getServiceName() const {
    return kServiceName;
}

NamespaceString MovePrimaryDonorService::getStateDocumentsNS() const {
    return NamespaceString::kMovePrimaryDonorNamespace;
}

ThreadPool::Limits MovePrimaryDonorService::getThreadPoolLimits() const {
    ThreadPool::Limits limits;
    limits.minThreads = gMovePrimaryDonorServiceMinThreadCount;
    limits.maxThreads = gMovePrimaryDonorServiceMaxThreadCount;
    return limits;
}

void MovePrimaryDonorService::checkIfConflictsWithOtherInstances(
    OperationContext* opCtx,
    BSONObj initialState,
    const std::vector<const repl::PrimaryOnlyService::Instance*>& existingInstances) {
    auto initialDoc = MovePrimaryDonorDocument::parse(
        IDLParserContext("MovePrimaryDonorCheckIfConflictsWithOtherInstances"), initialState);
    const auto& newMetadata = initialDoc.getMetadata();
    for (const auto& instance : existingInstances) {
        auto typed = checked_cast<const MovePrimaryDonor*>(instance);
        const auto& existingMetadata = typed->getMetadata();
        uassert(ErrorCodes::ConflictingOperationInProgress,
                str::stream() << "Existing movePrimary operation for database "
                              << newMetadata.getDatabaseName() << " is still ongoing",
                newMetadata.getDatabaseName() != existingMetadata.getDatabaseName());
    }
}

std::shared_ptr<repl::PrimaryOnlyService::Instance> MovePrimaryDonorService::constructInstance(
    BSONObj initialState) {
    auto initialDoc = MovePrimaryDonorDocument::parse(
        IDLParserContext("MovePrimaryDonorServiceConstructInstance"), initialState);
    return std::make_shared<MovePrimaryDonor>(_serviceContext,
                                              this,
                                              initialDoc,
                                              getInstanceCleanupExecutor(),
                                              _makeDependencies(initialDoc));
}

MovePrimaryDonorDependencies MovePrimaryDonorService::_makeDependencies(
    const MovePrimaryDonorDocument& initialDoc) {
    return {std::make_unique<MovePrimaryDonorExternalStateImpl>(initialDoc.getMetadata())};
}

MovePrimaryDonorCancelState::MovePrimaryDonorCancelState(const CancellationToken& stepdownToken)
    : _stepdownToken{stepdownToken},
      _abortSource{stepdownToken},
      _abortToken{_abortSource.token()} {}

const CancellationToken& MovePrimaryDonorCancelState::getStepdownToken() {
    return _stepdownToken;
}

const CancellationToken& MovePrimaryDonorCancelState::getAbortToken() {
    return _abortToken;
}

bool MovePrimaryDonorCancelState::isSteppingDown() const {
    return _stepdownToken.isCanceled();
}

void MovePrimaryDonorCancelState::abort() {
    _abortSource.cancel();
}

const Backoff MovePrimaryDonorRetryHelper::kBackoff{Seconds(1), Milliseconds::max()};

MovePrimaryDonorRetryHelper::MovePrimaryDonorRetryHelper(
    const MovePrimaryCommonMetadata& metadata,
    std::shared_ptr<executor::ScopedTaskExecutor> taskExecutor,
    MovePrimaryDonorCancelState* cancelState)
    : _metadata{metadata},
      _taskExecutor{taskExecutor},
      _markKilledExecutor{std::make_shared<ThreadPool>([] {
          ThreadPool::Options options;
          options.poolName = "MovePrimaryDonorRetryHelperCancelableOpCtxPool";
          options.minThreads = 1;
          options.maxThreads = 1;
          return options;
      }())},
      _cancelState{cancelState},
      _cancelOnStepdownFactory{_cancelState->getStepdownToken(), _markKilledExecutor},
      _cancelOnAbortFactory{_cancelState->getAbortToken(), _markKilledExecutor} {
    _markKilledExecutor->startup();
}

void MovePrimaryDonorRetryHelper::_handleTransientError(const std::string& operationName,
                                                        const Status& status) {
    LOGV2(7306301,
          "MovePrimaryDonor has encountered a transient error",
          "operation"_attr = operationName,
          "status"_attr = redact(status),
          "migrationId"_attr = _metadata.getMigrationId(),
          "databaseName"_attr = _metadata.getDatabaseName(),
          "toShard"_attr = _metadata.getToShardName());
}

void MovePrimaryDonorRetryHelper::_handleUnrecoverableError(const std::string& operationName,
                                                            const Status& status) {
    LOGV2(7306302,
          "MovePrimaryDonor has encountered an unrecoverable error",
          "operation"_attr = operationName,
          "status"_attr = redact(status),
          "migrationId"_attr = _metadata.getMigrationId(),
          "databaseName"_attr = _metadata.getDatabaseName(),
          "toShard"_attr = _metadata.getToShardName());
}

ExecutorFuture<void> MovePrimaryDonorRetryHelper::_waitForMajorityOrStepdown(
    const std::string& operationName) {
    auto cancelToken = _cancelState->getStepdownToken();
    return _untilStepdownOrSuccess(operationName, [cancelToken](const auto& factory) {
        auto opCtx = factory.makeOperationContext(&cc());
        auto client = opCtx->getClient();
        repl::ReplClientInfo::forClient(client).setLastOpToSystemLastOpTime(opCtx.get());
        auto opTime = repl::ReplClientInfo::forClient(client).getLastOp();
        return WaitForMajorityService::get(client->getServiceContext())
            .waitUntilMajority(opTime, cancelToken);
    });
}

MovePrimaryDonorExternalState::MovePrimaryDonorExternalState(
    const MovePrimaryCommonMetadata& metadata)
    : _metadata{metadata} {}


const MovePrimaryCommonMetadata& MovePrimaryDonorExternalState::getMetadata() const {
    return _metadata;
}

ShardId MovePrimaryDonorExternalState::getRecipientShardId() const {
    return ShardId{_metadata.getToShardName().toString()};
}

void MovePrimaryDonorExternalState::syncDataOnRecipient(OperationContext* opCtx) {
    syncDataOnRecipient(opCtx, boost::none);
}

void MovePrimaryDonorExternalState::syncDataOnRecipient(OperationContext* opCtx,
                                                        boost::optional<Timestamp> timestamp) {
    MovePrimaryRecipientSyncData request;
    request.setMovePrimaryCommonMetadata(getMetadata());
    request.setDbName(getMetadata().getDatabaseName().db());
    if (timestamp) {
        request.setReturnAfterReachingDonorTimestamp(*timestamp);
    }
    _runCommandOnRecipient(opCtx, request.toBSON({}));
}

void MovePrimaryDonorExternalState::abortMigrationOnRecipient(OperationContext* opCtx) {
    MovePrimaryRecipientAbortMigration request;
    request.setMovePrimaryCommonMetadata(getMetadata());
    request.setDbName(getMetadata().getDatabaseName().db());
    _runCommandOnRecipient(opCtx, request.toBSON({}));
}

void MovePrimaryDonorExternalState::forgetMigrationOnRecipient(OperationContext* opCtx) {
    MovePrimaryRecipientForgetMigration request;
    request.setMovePrimaryCommonMetadata(getMetadata());
    request.setDbName(getMetadata().getDatabaseName().db());
    _runCommandOnRecipient(opCtx, request.toBSON({}));
}

void MovePrimaryDonorExternalState::_runCommandOnRecipient(OperationContext* opCtx,
                                                           const BSONObj& command) {
    uassertStatusOK(runCommand(opCtx,
                               getRecipientShardId(),
                               ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                               DatabaseName::kAdmin.toString(),
                               command,
                               Shard::RetryPolicy::kNoRetry));
}

MovePrimaryDonorExternalStateImpl::MovePrimaryDonorExternalStateImpl(
    const MovePrimaryCommonMetadata& metadata)
    : MovePrimaryDonorExternalState{metadata} {}

StatusWith<Shard::CommandResponse> MovePrimaryDonorExternalStateImpl::runCommand(
    OperationContext* opCtx,
    const ShardId& shardId,
    const ReadPreferenceSetting& readPref,
    const std::string& dbName,
    const BSONObj& cmdObj,
    Shard::RetryPolicy retryPolicy) {
    auto shard = uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId));
    return shard->runCommand(opCtx, readPref, dbName, cmdObj, retryPolicy);
}

MovePrimaryDonor::MovePrimaryDonor(ServiceContext* serviceContext,
                                   MovePrimaryDonorService* donorService,
                                   MovePrimaryDonorDocument initialState,
                                   const std::shared_ptr<executor::TaskExecutor>& cleanupExecutor,
                                   MovePrimaryDonorDependencies dependencies)
    : _serviceContext{serviceContext},
      _donorService{donorService},
      _metadata{std::move(initialState.getMetadata())},
      _mutableFields{std::move(initialState.getMutableFields())},
      _metrics{MovePrimaryMetrics::initializeFrom(initialState, _serviceContext)},
      _cleanupExecutor{cleanupExecutor},
      _externalState{std::move(dependencies.externalState)} {
    if (auto abortReason = _mutableFields.getAbortReason()) {
        _abortReason = deserializeStatus(abortReason->getOwned());
    }
    _metrics->onStateTransition(boost::none, _getCurrentState());
}

SemiFuture<void> MovePrimaryDonor::run(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                       const CancellationToken& stepdownToken) noexcept {
    _initializeRun(executor, stepdownToken);
    _completionPromise.setFrom(
        _runDonorWorkflow().unsafeToInlineFuture().tapError([](const Status& status) {
            LOGV2(7306201, "MovePrimaryDonor encountered an error", "error"_attr = redact(status));
        }));
    return _completionPromise.getFuture().semi();
}

void MovePrimaryDonor::interrupt(Status status) {}

boost::optional<BSONObj> MovePrimaryDonor::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode connMode,
    MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept {
    return _metrics->reportForCurrentOp();
}

void MovePrimaryDonor::checkIfOptionsConflict(const BSONObj& stateDoc) const {
    auto otherDoc = MovePrimaryDonorDocument::parse(
        IDLParserContext("MovePrimaryDonorCheckIfOptionsConflict"), stateDoc);
    const auto& otherMetadata = otherDoc.getMetadata();
    const auto& metadata = getMetadata();
    invariant(metadata.getMigrationId() == otherMetadata.getMigrationId());
    uassert(ErrorCodes::ConflictingOperationInProgress,
            "Existing movePrimary operation exists with same id, but incompatible arguments",
            metadata.getDatabaseName() == otherMetadata.getDatabaseName() &&
                metadata.getToShardName() == otherMetadata.getToShardName());
}

const MovePrimaryCommonMetadata& MovePrimaryDonor::getMetadata() const {
    return _metadata;
}

SharedSemiFuture<void> MovePrimaryDonor::getReadyToBlockWritesFuture() const {
    return _progressedToReadyToBlockWritesPromise.getFuture();
}

SharedSemiFuture<void> MovePrimaryDonor::getDecisionFuture() const {
    return _progressedToDecisionPromise.getFuture();
}

SharedSemiFuture<void> MovePrimaryDonor::getCompletionFuture() const {
    return _completionPromise.getFuture();
}

MovePrimaryDonorStateEnum MovePrimaryDonor::_getCurrentState() const {
    stdx::unique_lock lock(_mutex);
    return _mutableFields.getState();
}

MovePrimaryDonorMutableFields MovePrimaryDonor::_getMutableFields() const {
    stdx::unique_lock lock(_mutex);
    return _mutableFields;
}

bool MovePrimaryDonor::_isAborted(WithLock) const {
    return _abortReason.has_value();
}

boost::optional<Status> MovePrimaryDonor::_getAbortReason() const {
    stdx::unique_lock lock(_mutex);
    return _abortReason;
}

Status MovePrimaryDonor::_getOperationStatus() const {
    return _getAbortReason().value_or(Status::OK());
}

MovePrimaryDonorDocument MovePrimaryDonor::_buildCurrentStateDocument() const {
    MovePrimaryDonorDocument doc;
    doc.setMetadata(getMetadata());
    doc.setMutableFields(_getMutableFields());
    doc.setId(getMetadata().getMigrationId());
    return doc;
}

void MovePrimaryDonor::_initializeRun(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                      const CancellationToken& stepdownToken) {
    stdx::unique_lock lock(_mutex);
    _taskExecutor = executor;
    _cancelState.emplace(stepdownToken);
    _retry.emplace(_metadata, _taskExecutor, _cancelState.get_ptr());
    if (_isAborted(lock)) {
        _cancelState->abort();
    }
}

ExecutorFuture<void> MovePrimaryDonor::_runDonorWorkflow() {
    using State = MovePrimaryDonorStateEnum;
    return _runOnTaskExecutor([] { pauseBeforeBeginningMovePrimaryDonorWorkflow.pauseWhileSet(); })
        .then([this] { return _transitionToState(State::kInitializing); })
        .then([this] { return _doInitializing(); })
        .then([this] { return _transitionToState(State::kCloning); })
        .then([this] { return _doCloning(); })
        .then([this] { return _transitionToState(State::kWaitingToBlockWrites); })
        .then([this] { return _doWaitingToBlockWrites(); })
        .then([this] { return _transitionToState(State::kBlockingWrites); })
        .then([this] { return _doBlockingWrites(); })
        .then([this] { return _transitionToState(State::kPrepared); })
        .onCompletion([this](Status result) {
            if (result.isOK()) {
                return _doPrepared();
            }
            abort(result);
            return _ensureAbortReasonSetInStateDocument()
                .then([this] { return _transitionToState(State::kAborted); })
                .then([this] { return _doAbort(); });
        })
        .then([this] { return _waitForForgetThenDoCleanup(); })
        .thenRunOn(_cleanupExecutor)
        .onCompletion([this, self = shared_from_this()](Status okOrStepdownError) {
            invariant(okOrStepdownError.isOK() || _cancelState->isSteppingDown());
            const auto& finalResult =
                _cancelState->isSteppingDown() ? okOrStepdownError : _getOperationStatus();
            _ensureProgressPromisesAreFulfilled(finalResult);
            return finalResult;
        });
}

bool MovePrimaryDonor::_allowedToAbortDuringStateTransition(
    MovePrimaryDonorStateEnum newState) const {
    switch (newState) {
        case MovePrimaryDonorStateEnum::kAborted:
        case MovePrimaryDonorStateEnum::kDone:
            return false;
        default:
            return true;
    }
}

ExecutorFuture<void> MovePrimaryDonor::_transitionToState(MovePrimaryDonorStateEnum newState) {
    auto op = fmt::format("transitionToState({})", MovePrimaryDonorState_serializer(newState));
    auto action = [this, newState](const auto& factory) {
        auto opCtx = factory.makeOperationContext(&cc());
        _tryTransitionToStateOnce(opCtx.get(), newState);
    };
    if (_allowedToAbortDuringStateTransition(newState)) {
        return _retry->untilAbortOrMajorityCommit(op, action);
    }
    return _retry->untilStepdownOrMajorityCommit(op, action);
}

void MovePrimaryDonor::_tryTransitionToStateOnce(OperationContext* opCtx,
                                                 MovePrimaryDonorStateEnum newState) {
    auto oldState = _getCurrentState();
    if (oldState >= newState) {
        return;
    }
    auto newDocument = _buildCurrentStateDocument();
    newDocument.getMutableFields().setState(newState);
    evaluatePauseDuringStateTransitionFailpoints(StateTransitionProgress::kBefore, newState);

    _updateOnDiskState(opCtx, newDocument);

    evaluatePauseDuringStateTransitionFailpoints(StateTransitionProgress::kPartial, newState);

    _updateInMemoryState(newDocument);
    _metrics->onStateTransition(oldState, newState);

    LOGV2(7306300,
          "MovePrimaryDonor transitioned state",
          "oldState"_attr = MovePrimaryDonorState_serializer(oldState),
          "newState"_attr = MovePrimaryDonorState_serializer(newState),
          "migrationId"_attr = _metadata.getMigrationId(),
          "databaseName"_attr = _metadata.getDatabaseName(),
          "toShard"_attr = _metadata.getToShardName());

    evaluatePauseDuringStateTransitionFailpoints(StateTransitionProgress::kAfter, newState);
}

void MovePrimaryDonor::_updateOnDiskState(OperationContext* opCtx,
                                          const MovePrimaryDonorDocument& newStateDocument) {
    PersistentTaskStore<MovePrimaryDonorDocument> store(_donorService->getStateDocumentsNS());
    auto oldState = _getCurrentState();
    auto newState = newStateDocument.getMutableFields().getState();
    if (oldState == MovePrimaryDonorStateEnum::kUnused) {
        store.add(opCtx, newStateDocument, WriteConcerns::kLocalWriteConcern);
    } else if (newState == MovePrimaryDonorStateEnum::kDone) {
        store.remove(opCtx,
                     BSON(MovePrimaryDonorDocument::kIdFieldName << _metadata.getMigrationId()),
                     WriteConcerns::kLocalWriteConcern);
    } else {
        store.update(opCtx,
                     BSON(MovePrimaryDonorDocument::kIdFieldName << _metadata.getMigrationId()),
                     BSON("$set" << BSON(MovePrimaryDonorDocument::kMutableFieldsFieldName
                                         << newStateDocument.getMutableFields().toBSON())),
                     WriteConcerns::kLocalWriteConcern);
    }
}

void MovePrimaryDonor::_updateInMemoryState(const MovePrimaryDonorDocument& newStateDocument) {
    stdx::unique_lock lock(_mutex);
    _mutableFields = newStateDocument.getMutableFields();
}

ExecutorFuture<void> MovePrimaryDonor::_doInitializing() {
    return _retry->untilAbortOrMajorityCommit("doInitializing()", [](const auto& factory) {
        // TODO: SERVER-74757
    });
}

ExecutorFuture<void> MovePrimaryDonor::_doNothing() {
    return _runOnTaskExecutor([] {});
}

ExecutorFuture<void> MovePrimaryDonor::_doCloning() {
    if (_getCurrentState() > MovePrimaryDonorStateEnum::kCloning) {
        return _doNothing();
    }
    return _retry->untilAbortOrMajorityCommit("doCloning()", [this](const auto& factory) {
        auto opCtx = factory.makeOperationContext(&cc());
        _externalState->syncDataOnRecipient(opCtx.get());
    });
}

ExecutorFuture<void> MovePrimaryDonor::_doWaitingToBlockWrites() {
    if (_getCurrentState() > MovePrimaryDonorStateEnum::kWaitingToBlockWrites) {
        return _doNothing();
    }
    return _waitUntilReadyToBlockWrites()
        .then([this] { return _waitUntilCurrentlyBlockingWrites(); })
        .then([this](Timestamp blockingWritesTimestamp) {
            return _persistBlockingWritesTimestamp(blockingWritesTimestamp);
        });
}

ExecutorFuture<void> MovePrimaryDonor::_waitUntilReadyToBlockWrites() {
    return _runOnTaskExecutor([this] {
        // TODO SERVER-74933: Use commit monitor to determine when to engage critical section.
        LOGV2(7306500,
              "MovePrimaryDonor ready to block writes",
              "migrationId"_attr = _metadata.getMigrationId(),
              "databaseName"_attr = _metadata.getDatabaseName(),
              "toShard"_attr = _metadata.getToShardName());

        _progressedToReadyToBlockWritesPromise.setFrom(Status::OK());
    });
}

ExecutorFuture<Timestamp> MovePrimaryDonor::_waitUntilCurrentlyBlockingWrites() {
    return _runOnTaskExecutor([this] {
        return future_util::withCancellation(_currentlyBlockingWritesPromise.getFuture(),
                                             _cancelState->getAbortToken());
    });
}

void MovePrimaryDonor::onBeganBlockingWrites(StatusWith<Timestamp> blockingWritesTimestamp) {
    _currentlyBlockingWritesPromise.setFrom(blockingWritesTimestamp);
}

void MovePrimaryDonor::onReadyToForget() {
    _readyToForgetPromise.setFrom(Status::OK());
}

void MovePrimaryDonor::abort(Status reason) {
    invariant(!reason.isOK());
    stdx::unique_lock lock(_mutex);
    if (_isAborted(lock)) {
        return;
    }

    _abortReason = reason;
    if (_cancelState) {
        _cancelState->abort();
    }

    LOGV2(7306700,
          "MovePrimaryDonor has received signal to abort",
          "reason"_attr = redact(reason),
          "migrationId"_attr = _metadata.getMigrationId(),
          "databaseName"_attr = _metadata.getDatabaseName(),
          "toShard"_attr = _metadata.getToShardName());
}

bool MovePrimaryDonor::isAborted() const {
    stdx::unique_lock lock(_mutex);
    return _isAborted(lock);
}

ExecutorFuture<void> MovePrimaryDonor::_persistBlockingWritesTimestamp(
    Timestamp blockingWritesTimestamp) {
    return _retry->untilAbortOrMajorityCommit(
        fmt::format("persistBlockingWritesTimestamp({})", blockingWritesTimestamp.toString()),
        [this, blockingWritesTimestamp](const auto& factory) {
            auto opCtx = factory.makeOperationContext(&cc());
            auto newStateDocument = _buildCurrentStateDocument();
            newStateDocument.getMutableFields().setBlockingWritesTimestamp(blockingWritesTimestamp);
            _updateOnDiskState(opCtx.get(), newStateDocument);
            _updateInMemoryState(newStateDocument);

            LOGV2(7306501,
                  "MovePrimaryDonor persisted block timestamp",
                  "blockingWritesTimestamp"_attr = blockingWritesTimestamp,
                  "migrationId"_attr = _metadata.getMigrationId(),
                  "databaseName"_attr = _metadata.getDatabaseName(),
                  "toShard"_attr = _metadata.getToShardName());
        });
}

ExecutorFuture<void> MovePrimaryDonor::_doBlockingWrites() {
    if (_getCurrentState() > MovePrimaryDonorStateEnum::kBlockingWrites) {
        return _doNothing();
    }
    return _retry->untilAbortOrMajorityCommit("doBlockingWrites()", [this](const auto& factory) {
        auto opCtx = factory.makeOperationContext(&cc());
        auto timestamp = _getMutableFields().getBlockingWritesTimestamp();
        invariant(timestamp);
        _externalState->syncDataOnRecipient(opCtx.get(), *timestamp);
    });
}

ExecutorFuture<void> MovePrimaryDonor::_doPrepared() {
    return _runOnTaskExecutor([this] { _progressedToDecisionPromise.setFrom(Status::OK()); });
}

ExecutorFuture<void> MovePrimaryDonor::_waitForForgetThenDoCleanup() {
    return _runOnTaskExecutor([this] {
               return future_util::withCancellation(_readyToForgetPromise.getFuture(),
                                                    _cancelState->getStepdownToken());
           })
        .then([this] { pauseBeforeBeginningMovePrimaryDonorCleanup.pauseWhileSet(); })
        .then([this] { return _doCleanup(); })
        .then([this] { return _transitionToState(MovePrimaryDonorStateEnum::kDone); });
}

ExecutorFuture<void> MovePrimaryDonor::_doCleanup() {
    return _doAbortIfRequired().then([this] { return _doForget(); });
}

ExecutorFuture<void> MovePrimaryDonor::_doAbortIfRequired() {
    if (!isAborted()) {
        return _doNothing();
    }
    return _doAbort();
}

ExecutorFuture<void> MovePrimaryDonor::_ensureAbortReasonSetInStateDocument() {
    return _runOnTaskExecutor([this] {
        auto doc = _buildCurrentStateDocument();
        if (doc.getMutableFields().getAbortReason()) {
            return;
        }
        auto reason = _getAbortReason();
        invariant(reason);
        BSONObjBuilder bob;
        reason->serializeErrorToBSON(&bob);
        doc.getMutableFields().setAbortReason(bob.obj());
        _updateInMemoryState(doc);
    });
}

ExecutorFuture<void> MovePrimaryDonor::_doAbort() {
    return _retry->untilStepdownOrMajorityCommit("doAbort()", [this](const auto& factory) {
        _ensureProgressPromisesAreFulfilled(_getOperationStatus());
        auto opCtx = factory.makeOperationContext(&cc());
        _externalState->abortMigrationOnRecipient(opCtx.get());
    });
}

ExecutorFuture<void> MovePrimaryDonor::_doForget() {
    return _retry->untilStepdownOrMajorityCommit("doForget()", [this](const auto& factory) {
        auto opCtx = factory.makeOperationContext(&cc());
        _externalState->forgetMigrationOnRecipient(opCtx.get());
    });
}

void MovePrimaryDonor::_ensureProgressPromisesAreFulfilled(Status result) {
    if (!_progressedToReadyToBlockWritesPromise.getFuture().isReady()) {
        _progressedToReadyToBlockWritesPromise.setFrom(result);
    }
    if (!_progressedToDecisionPromise.getFuture().isReady()) {
        _progressedToDecisionPromise.setFrom(result);
    }
}

}  // namespace mongo
