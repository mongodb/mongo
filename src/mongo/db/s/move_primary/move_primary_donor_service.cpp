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
    return std::make_shared<MovePrimaryDonor>(
        _serviceContext, this, initialDoc, makeDependencies(initialDoc));
}

MovePrimaryDonorDependencies MovePrimaryDonorService::makeDependencies(
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

void MovePrimaryDonorRetryHelper::handleTransientError(const std::string& operationName,
                                                       const Status& status) {
    LOGV2(7306301,
          "MovePrimaryDonor has encountered a transient error",
          "operation"_attr = operationName,
          "status"_attr = redact(status),
          "migrationId"_attr = _metadata.get_id(),
          "databaseName"_attr = _metadata.getDatabaseName(),
          "toShard"_attr = _metadata.getShardName());
}

void MovePrimaryDonorRetryHelper::handleUnrecoverableError(const std::string& operationName,
                                                           const Status& status) {
    LOGV2(7306302,
          "MovePrimaryDonor has encountered an unrecoverable error",
          "operation"_attr = operationName,
          "status"_attr = redact(status),
          "migrationId"_attr = _metadata.get_id(),
          "databaseName"_attr = _metadata.getDatabaseName(),
          "toShard"_attr = _metadata.getShardName());
}

MovePrimaryDonorExternalState::MovePrimaryDonorExternalState(
    const MovePrimaryCommonMetadata& metadata)
    : _metadata{metadata} {}


const MovePrimaryCommonMetadata& MovePrimaryDonorExternalState::getMetadata() const {
    return _metadata;
}

ShardId MovePrimaryDonorExternalState::getRecipientShardId() const {
    return ShardId{_metadata.getShardName().toString()};
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
    runCommandOnRecipient(opCtx, request.toBSON({}));
}

void MovePrimaryDonorExternalState::runCommandOnRecipient(OperationContext* opCtx,
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
                                   MovePrimaryDonorDependencies dependencies)
    : _serviceContext{serviceContext},
      _donorService{donorService},
      _metadata{std::move(initialState.getMetadata())},
      _mutableFields{std::move(initialState.getMutableFields())},
      _metrics{MovePrimaryMetrics::initializeFrom(initialState, _serviceContext)},
      _externalState{std::move(dependencies.externalState)} {
    _metrics->onStateTransition(boost::none, getCurrentState());
}

SemiFuture<void> MovePrimaryDonor::run(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                       const CancellationToken& stepdownToken) noexcept {
    initializeRun(executor, stepdownToken);
    _completionPromise.setFrom(
        runDonorWorkflow().unsafeToInlineFuture().tapError([](const Status& status) {
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
    invariant(metadata.get_id() == otherMetadata.get_id());
    uassert(ErrorCodes::ConflictingOperationInProgress,
            "Existing movePrimary operation exists with same id, but incompatible arguments",
            metadata.getDatabaseName() == otherMetadata.getDatabaseName() &&
                metadata.getShardName() == otherMetadata.getShardName());
}

const MovePrimaryCommonMetadata& MovePrimaryDonor::getMetadata() const {
    return _metadata;
}

SharedSemiFuture<void> MovePrimaryDonor::getReadyToBlockWritesFuture() const {
    return _readyToBlockWritesPromise.getFuture();
}

SharedSemiFuture<void> MovePrimaryDonor::getCompletionFuture() const {
    return _completionPromise.getFuture();
}

MovePrimaryDonorStateEnum MovePrimaryDonor::getCurrentState() const {
    stdx::unique_lock lock(_mutex);
    return _mutableFields.getState();
}

MovePrimaryDonorMutableFields MovePrimaryDonor::getMutableFields() const {
    stdx::unique_lock lock(_mutex);
    return _mutableFields;
}

MovePrimaryDonorDocument MovePrimaryDonor::buildCurrentStateDocument() const {
    MovePrimaryDonorDocument doc;
    doc.setMetadata(getMetadata());
    doc.setMutableFields(getMutableFields());
    return doc;
}

void MovePrimaryDonor::initializeRun(std::shared_ptr<executor::ScopedTaskExecutor> executor,
                                     const CancellationToken& stepdownToken) {
    stdx::unique_lock lock(_mutex);
    _taskExecutor = executor;
    _cancelState.emplace(stepdownToken);
    _retry.emplace(_metadata, _taskExecutor, _cancelState.get_ptr());
}

ExecutorFuture<void> MovePrimaryDonor::runDonorWorkflow() {
    return runOnTaskExecutor([] { pauseBeforeBeginningMovePrimaryDonorWorkflow.pauseWhileSet(); })
        .then([this] { return transitionToState(MovePrimaryDonorStateEnum::kInitializing); })
        .then([this] { return doInitializing(); })
        .then([this] { return transitionToState(MovePrimaryDonorStateEnum::kCloning); })
        .then([this] { return doCloning(); })
        .then(
            [this] { return transitionToState(MovePrimaryDonorStateEnum::kWaitingToBlockWrites); })
        .then([this] { return doWaitingToBlockWrites(); })
        .then([this] { return transitionToState(MovePrimaryDonorStateEnum::kBlockingWrites); })
        .then([this] { return doBlockingWrites(); })
        .then([this] { return transitionToState(MovePrimaryDonorStateEnum::kPrepared); });
}

ExecutorFuture<void> MovePrimaryDonor::transitionToState(MovePrimaryDonorStateEnum newState) {
    return _retry->untilAbortOrSuccess(
        fmt::format("transitionToState({})", MovePrimaryDonorState_serializer(newState)),
        [this, newState](const auto& factory) {
            auto oldState = getCurrentState();
            if (oldState >= newState) {
                return;
            }
            auto opCtx = factory.makeOperationContext(&cc());
            auto newDocument = buildCurrentStateDocument();
            newDocument.getMutableFields().setState(newState);
            evaluatePauseDuringStateTransitionFailpoints(StateTransitionProgress::kBefore,
                                                         newState);

            updateOnDiskState(opCtx.get(), newDocument);

            evaluatePauseDuringStateTransitionFailpoints(StateTransitionProgress::kPartial,
                                                         newState);

            updateInMemoryState(newDocument);
            _metrics->onStateTransition(oldState, newState);

            evaluatePauseDuringStateTransitionFailpoints(StateTransitionProgress::kAfter, newState);

            LOGV2(7306300,
                  "MovePrimaryDonor transitioned state",
                  "oldState"_attr = MovePrimaryDonorState_serializer(oldState),
                  "newState"_attr = MovePrimaryDonorState_serializer(newState),
                  "migrationId"_attr = _metadata.get_id(),
                  "databaseName"_attr = _metadata.getDatabaseName(),
                  "toShard"_attr = _metadata.getShardName());
        });
}

void MovePrimaryDonor::updateOnDiskState(OperationContext* opCtx,
                                         const MovePrimaryDonorDocument& newStateDocument) {
    PersistentTaskStore<MovePrimaryDonorDocument> store(_donorService->getStateDocumentsNS());
    auto state = newStateDocument.getMutableFields().getState();
    if (state == MovePrimaryDonorStateEnum::kInitializing) {
        store.add(opCtx, newStateDocument);
    } else {
        store.update(opCtx,
                     BSON(MovePrimaryDonorDocument::k_idFieldName << _metadata.get_id()),
                     BSON("$set" << BSON(MovePrimaryDonorDocument::kMutableFieldsFieldName
                                         << newStateDocument.getMutableFields().toBSON())));
    }
}

void MovePrimaryDonor::updateInMemoryState(const MovePrimaryDonorDocument& newStateDocument) {
    stdx::unique_lock lock(_mutex);
    _mutableFields = newStateDocument.getMutableFields();
}

ExecutorFuture<void> MovePrimaryDonor::doInitializing() {
    return _retry->untilAbortOrSuccess("doInitializing()", [](const auto& factory) {
        // TODO: SERVER-74757
    });
}

ExecutorFuture<void> MovePrimaryDonor::doNothing() {
    return runOnTaskExecutor([] {});
}

ExecutorFuture<void> MovePrimaryDonor::doCloning() {
    return _retry->untilAbortOrSuccess("doCloning()", [this](const auto& factory) {
        auto opCtx = factory.makeOperationContext(&cc());
        _externalState->syncDataOnRecipient(opCtx.get());
    });
}

ExecutorFuture<void> MovePrimaryDonor::doWaitingToBlockWrites() {
    return runOnTaskExecutor([this] {
        if (getCurrentState() > MovePrimaryDonorStateEnum::kWaitingToBlockWrites) {
            return doNothing();
        }
        return waitUntilReadyToBlockWrites()
            .then([this] { return waitUntilCurrentlyBlockingWrites(); })
            .then([this](Timestamp blockingWritesTimestamp) {
                return persistBlockingWritesTimestamp(blockingWritesTimestamp);
            });
    });
}

ExecutorFuture<void> MovePrimaryDonor::waitUntilReadyToBlockWrites() {
    return runOnTaskExecutor([this] {
        // TODO SERVER-74933: Use commit monitor to determine when to engage critical section.
        LOGV2(7306500,
              "MovePrimaryDonor ready to block writes",
              "migrationId"_attr = _metadata.get_id(),
              "databaseName"_attr = _metadata.getDatabaseName(),
              "toShard"_attr = _metadata.getShardName());

        _readyToBlockWritesPromise.setFrom(Status::OK());
    });
}

ExecutorFuture<Timestamp> MovePrimaryDonor::waitUntilCurrentlyBlockingWrites() {
    return runOnTaskExecutor([this] { return _currentlyBlockingWritesPromise.getFuture().get(); });
}

void MovePrimaryDonor::onBeganBlockingWrites(StatusWith<Timestamp> blockingWritesTimestamp) {
    _currentlyBlockingWritesPromise.setFrom(blockingWritesTimestamp);
}

ExecutorFuture<void> MovePrimaryDonor::persistBlockingWritesTimestamp(
    Timestamp blockingWritesTimestamp) {
    return _retry->untilAbortOrSuccess(
        fmt::format("persistBlockingWritesTimestamp({})", blockingWritesTimestamp.toString()),
        [this, blockingWritesTimestamp](const auto& factory) {
            auto opCtx = factory.makeOperationContext(&cc());
            auto newStateDocument = buildCurrentStateDocument();
            newStateDocument.getMutableFields().setBlockingWritesTimestamp(blockingWritesTimestamp);
            updateOnDiskState(opCtx.get(), newStateDocument);
            updateInMemoryState(newStateDocument);

            LOGV2(7306501,
                  "MovePrimaryDonor persisted block timestamp",
                  "blockingWritesTimestamp"_attr = blockingWritesTimestamp,
                  "migrationId"_attr = _metadata.get_id(),
                  "databaseName"_attr = _metadata.getDatabaseName(),
                  "toShard"_attr = _metadata.getShardName());
        });
}

ExecutorFuture<void> MovePrimaryDonor::doBlockingWrites() {
    return _retry->untilAbortOrSuccess("doBlockingWrites()", [this](const auto& factory) {
        auto opCtx = factory.makeOperationContext(&cc());
        auto timestamp = getMutableFields().getBlockingWritesTimestamp();
        invariant(timestamp);
        _externalState->syncDataOnRecipient(opCtx.get(), *timestamp);
    });
}


}  // namespace mongo
