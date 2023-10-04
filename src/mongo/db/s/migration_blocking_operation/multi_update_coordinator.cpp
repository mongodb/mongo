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

#include "mongo/db/s/migration_blocking_operation/multi_update_coordinator.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/s/migration_blocking_operation/multi_update_coordinator_server_parameters_gen.h"
#include "mongo/db/s/primary_only_service_helpers/pause_during_state_transition_fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {
MONGO_FAIL_POINT_DEFINE(pauseDuringMultiUpdateCoordinatorStateTransition);
MONGO_FAIL_POINT_DEFINE(pauseDuringMultiUpdateCoordinatorStateTransitionAlternate);
primary_only_service_helpers::PauseDuringStateTransitionFailPoint<MultiUpdateCoordinatorStateEnum>
    pauseDuringStateTransitions{
        {pauseDuringMultiUpdateCoordinatorStateTransition,
         pauseDuringMultiUpdateCoordinatorStateTransitionAlternate},
        [](StringData state) {
            IDLParserContext ectx(
                "pauseDuringMultiUpdateCoordinatorStateTransition::readStateArgument");
            return MultiUpdateCoordinatorState_parse(ectx, state);
        }};
}  // namespace

MultiUpdateCoordinatorService::MultiUpdateCoordinatorService(ServiceContext* serviceContext)
    : PrimaryOnlyService{serviceContext}, _serviceContext{serviceContext} {}

StringData MultiUpdateCoordinatorService::getServiceName() const {
    return kServiceName;
}

NamespaceString MultiUpdateCoordinatorService::getStateDocumentsNS() const {
    return NamespaceString::kMultiUpdateCoordinatorsNamespace;
}

ThreadPool::Limits MultiUpdateCoordinatorService::getThreadPoolLimits() const {
    ThreadPool::Limits limits;
    limits.minThreads = gMultiUpdateCoordinatorServiceMinThreadCount;
    limits.maxThreads = gMultiUpdateCoordinatorServiceMaxThreadCount;
    return limits;
}

void MultiUpdateCoordinatorService::checkIfConflictsWithOtherInstances(
    OperationContext* opCtx,
    BSONObj initialState,
    const std::vector<const repl::PrimaryOnlyService::Instance*>& existingInstances) {}

std::shared_ptr<repl::PrimaryOnlyService::Instance>
MultiUpdateCoordinatorService::constructInstance(BSONObj initialState) {
    auto initialDocument = MultiUpdateCoordinatorDocument::parse(
        IDLParserContext("MultiUpdateCoordinatorServiceConstructInstance"), initialState);
    return std::make_shared<MultiUpdateCoordinatorInstance>(this, std::move(initialDocument));
}

MultiUpdateCoordinatorInstance::MultiUpdateCoordinatorInstance(
    const MultiUpdateCoordinatorService* const service,
    MultiUpdateCoordinatorDocument initialDocument)
    : _service{service},
      _metadata{std::move(initialDocument.getMetadata())},
      _mutableFields{std::move(initialDocument.getMutableFields())} {}

SharedSemiFuture<void> MultiUpdateCoordinatorInstance::getCompletionFuture() const {
    return _completionPromise.getFuture();
}

SemiFuture<void> MultiUpdateCoordinatorInstance::run(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& stepdownToken) noexcept {
    _initializeRun(executor, stepdownToken);
    _completionPromise.setFrom(
        _runWorkflow().unsafeToInlineFuture().tapError([this](const Status& status) {
            LOGV2(8126401,
                  "MultiUpdateCoordinatorInstance encountered an error",
                  "error"_attr = redact(status),
                  "metadata"_attr = getMetadata());
        }));
    return _completionPromise.getFuture().semi();
}

void MultiUpdateCoordinatorInstance::interrupt(Status status) {}

boost::optional<BSONObj> MultiUpdateCoordinatorInstance::reportForCurrentOp(
    MongoProcessInterface::CurrentOpConnectionsMode connMode,
    MongoProcessInterface::CurrentOpSessionsMode sessionMode) noexcept {
    return _buildCurrentStateDocument().toBSON();
}

void MultiUpdateCoordinatorInstance::checkIfOptionsConflict(const BSONObj& stateDoc) const {}

const MultiUpdateCoordinatorMetadata& MultiUpdateCoordinatorInstance::getMetadata() const {
    return _metadata;
}

MultiUpdateCoordinatorMutableFields MultiUpdateCoordinatorInstance::_getMutableFields() const {
    stdx::unique_lock lock(_mutex);
    return _mutableFields;
}

MultiUpdateCoordinatorStateEnum MultiUpdateCoordinatorInstance::_getCurrentState() const {
    return _getMutableFields().getState();
}

MultiUpdateCoordinatorDocument MultiUpdateCoordinatorInstance::_buildCurrentStateDocument() const {
    MultiUpdateCoordinatorDocument document;
    document.setMetadata(getMetadata());
    document.setMutableFields(_getMutableFields());
    return document;
}

void MultiUpdateCoordinatorInstance::_initializeRun(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& stepdownToken) {
    _taskExecutor = executor;
    _cancelState.emplace(stepdownToken);
    _retry.emplace(
        _service->getServiceName(), _taskExecutor, _cancelState.get_ptr(), getMetadata().toBSON());
}

ExecutorFuture<void> MultiUpdateCoordinatorInstance::_runWorkflow() {
    using State = MultiUpdateCoordinatorStateEnum;
    return _transitionToState(State::kBlockMigrations)
        .then([this] { return _transitionToState(State::kPerformUpdate); })
        .then([this] { return _transitionToState(State::kCleanup); })
        .then([this] { return _transitionToState(State::kDone); });
}

ExecutorFuture<void> MultiUpdateCoordinatorInstance::_transitionToState(
    MultiUpdateCoordinatorStateEnum newState) {
    return _retry->untilAbortOrMajorityCommit(
        fmt::format("transitionToState({})", MultiUpdateCoordinatorState_serializer(newState)),
        [this, newState](const auto& factory) {
            auto oldState = _getCurrentState();
            if (oldState >= newState) {
                return;
            }
            auto opCtx = factory.makeOperationContext(&cc());
            auto newDocument = _buildCurrentStateDocument();
            newDocument.getMutableFields().setState(newState);
            pauseDuringStateTransitions.evaluate(StateTransitionProgressEnum::kBefore, newState);
            _updateOnDiskState(opCtx.get(), newDocument);
            pauseDuringStateTransitions.evaluate(StateTransitionProgressEnum::kPartial, newState);
            _updateInMemoryState(newDocument);
            pauseDuringStateTransitions.evaluate(StateTransitionProgressEnum::kAfter, newState);
        });
}

void MultiUpdateCoordinatorInstance::_updateOnDiskState(
    OperationContext* opCtx, const MultiUpdateCoordinatorDocument& newStateDocument) {
    PersistentTaskStore<MultiUpdateCoordinatorDocument> store(_service->getStateDocumentsNS());
    auto oldState = _getCurrentState();
    auto newState = newStateDocument.getMutableFields().getState();
    if (oldState == MultiUpdateCoordinatorStateEnum::kUnused) {
        store.add(opCtx, newStateDocument, WriteConcerns::kLocalWriteConcern);
    } else if (newState == MultiUpdateCoordinatorStateEnum::kDone) {
        store.remove(opCtx,
                     BSON(MultiUpdateCoordinatorDocument::kIdFieldName << _metadata.getId()),
                     WriteConcerns::kLocalWriteConcern);
    } else {
        store.update(opCtx,
                     BSON(MultiUpdateCoordinatorDocument::kIdFieldName << _metadata.getId()),
                     BSON("$set" << BSON(MultiUpdateCoordinatorDocument::kMutableFieldsFieldName
                                         << newStateDocument.getMutableFields().toBSON())),
                     WriteConcerns::kLocalWriteConcern);
    }
}

void MultiUpdateCoordinatorInstance::_updateInMemoryState(
    const MultiUpdateCoordinatorDocument& newStateDocument) {
    stdx::unique_lock lock(_mutex);
    _mutableFields = newStateDocument.getMutableFields();
}

}  // namespace mongo
