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
#include "mongo/db/s/move_primary/move_primary_server_parameters_gen.h"

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
    return std::make_shared<MovePrimaryDonor>(
        _serviceContext,
        this,
        MovePrimaryDonorDocument::parse(
            IDLParserContext("MovePrimaryDonorServiceConstructInstance"), initialState));
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

MovePrimaryDonorRetryHelper::MovePrimaryDonorRetryHelper(
    std::shared_ptr<executor::ScopedTaskExecutor> taskExecutor,
    MovePrimaryDonorCancelState* cancelState)
    : _taskExecutor{taskExecutor},
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

void MovePrimaryDonorRetryHelper::handleTransientError(const Status& status) {}
void MovePrimaryDonorRetryHelper::handleUnrecoverableError(const Status& status) {}

MovePrimaryDonor::MovePrimaryDonor(ServiceContext* serviceContext,
                                   MovePrimaryDonorService* donorService,
                                   MovePrimaryDonorDocument initialState)
    : _serviceContext{serviceContext},
      _donorService{donorService},
      _metadata{std::move(initialState.getMetadata())},
      _mutableFields{std::move(initialState.getMutableFields())},
      _metrics{MovePrimaryMetrics::initializeFrom(initialState, _serviceContext)} {
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
    _retry.emplace(_taskExecutor, _cancelState.get_ptr());
}

ExecutorFuture<void> MovePrimaryDonor::runDonorWorkflow() {
    return runOnTaskExecutor([] { pauseBeforeBeginningMovePrimaryDonorWorkflow.pauseWhileSet(); })
        .then([this] { return transitionToState(MovePrimaryDonorStateEnum::kInitializing); })
        .then([this] { return doInitializing(); });
}

ExecutorFuture<void> MovePrimaryDonor::transitionToState(MovePrimaryDonorStateEnum newState) {
    return _retry->untilAbortOrSuccess([this, newState](const auto& factory) {
        auto oldState = getCurrentState();
        if (oldState >= newState) {
            return;
        }
        auto opCtx = factory.makeOperationContext(&cc());
        auto newDocument = buildCurrentStateDocument();
        newDocument.getMutableFields().setState(newState);
        evaluatePauseDuringStateTransitionFailpoints(StateTransitionProgress::kBefore, newState);

        updateOnDiskState(opCtx.get(), newDocument);

        evaluatePauseDuringStateTransitionFailpoints(StateTransitionProgress::kPartial, newState);

        updateInMemoryState(newDocument);
        _metrics->onStateTransition(oldState, newState);

        evaluatePauseDuringStateTransitionFailpoints(StateTransitionProgress::kAfter, newState);
    });
}

void MovePrimaryDonor::updateOnDiskState(OperationContext* opCtx,
                                         const MovePrimaryDonorDocument& newStateDocument) {
    PersistentTaskStore<MovePrimaryDonorDocument> store(_donorService->getStateDocumentsNS());
    store.add(opCtx, newStateDocument);
}

void MovePrimaryDonor::updateInMemoryState(const MovePrimaryDonorDocument& newStateDocument) {
    stdx::unique_lock lock(_mutex);
    _mutableFields = newStateDocument.getMutableFields();
}

ExecutorFuture<void> MovePrimaryDonor::doInitializing() {
    return _retry->untilAbortOrSuccess([](const auto& factory) {
        // TODO: SERVER-74757
    });
}

}  // namespace mongo
