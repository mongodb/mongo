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
#include "mongo/db/cluster_command_translations.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/s/migration_blocking_operation/multi_update_coordinator_server_parameters_gen.h"
#include "mongo/db/s/primary_only_service_helpers/pause_during_state_transition_fail_point.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/rpc/protocol.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/s/service_entry_point_mongos.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {
MONGO_FAIL_POINT_DEFINE(pauseDuringMultiUpdateCoordinatorStateTransition);
MONGO_FAIL_POINT_DEFINE(pauseDuringMultiUpdateCoordinatorStateTransitionAlternate);
using State = MultiUpdateCoordinatorStateEnum;

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

Future<DbResponse> MultiUpdateCoordinatorExternalStateImpl::sendClusterUpdateCommandToShards(
    OperationContext* opCtx, const Message& message) const {
    return ServiceEntryPointMongos::handleRequestImpl(opCtx, message);
}

MultiUpdateCoordinatorService::MultiUpdateCoordinatorService(
    ServiceContext* serviceContext,
    std::unique_ptr<MultiUpdateCoordinatorExternalStateFactory> factory)
    : PrimaryOnlyService{serviceContext},
      _serviceContext{serviceContext},
      _externalStateFactory{std::move(factory)} {}

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
      _mutableFields{std::move(initialDocument.getMutableFields())},
      _externalState{service->_externalStateFactory->createExternalState()},
      _cmdResponse{_mutableFields.getResult()} {}

SharedSemiFuture<BSONObj> MultiUpdateCoordinatorInstance::getCompletionFuture() const {
    return _completionPromise.getFuture();
}

SemiFuture<void> MultiUpdateCoordinatorInstance::run(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& stepdownToken) noexcept {
    _initializeRun(executor, stepdownToken);

    return _transitionToState(State::kBlockMigrations)
        .then([this] { return _beginOperation(); })
        .then([this] { return _performUpdate(); })
        .then([this] { return _checkForPendingUpdates(); })
        .then([this] { return _cleanup(); })
        .onCompletion([this, self = shared_from_this()](Status operationStatus) {
            return _retry
                ->untilStepdownOrMajorityCommit("MultiUpdateCoordinator::endOperation",
                                                [this](const auto& factory) { _endOperation(); })
                .then([this] { return _transitionToState(State::kDone); })
                .onCompletion([this, operationStatus](Status cleanupStatus) {
                    // Validating that cleanup status is ok is redundant because
                    // untilStepdownOrMajorityCommit() will only stop retrying on a stepdown, which
                    // means we will directly run the instance cleanup executor chain with a non-ok
                    // status.
                    invariant(cleanupStatus.isOK());
                    return operationStatus;
                });
        })
        .thenRunOn(_service->getInstanceCleanupExecutor())
        .onCompletion([this, self = shared_from_this()](Status status) {
            if (status.isOK()) {
                invariant(_cmdResponse);
                _completionPromise.emplaceValue(_cmdResponse.get());
            } else {
                _completionPromise.setError(status);
            }
        })
        .semi();
}

ExecutorFuture<void> MultiUpdateCoordinatorInstance::_beginOperation() {
    if (_getCurrentState() > State::kBlockMigrations) {
        return ExecutorFuture<void>(**_taskExecutor, Status::OK());
    }

    // TODO(SERVER-81265): Chain call to beginOperation();
    return ExecutorFuture<void>(**_taskExecutor, Status::OK());
}

ExecutorFuture<void> MultiUpdateCoordinatorInstance::_performUpdate() {
    if (_getCurrentState() > State::kPerformUpdate) {
        return ExecutorFuture<void>(**_taskExecutor, Status::OK());
    }

    return _transitionToState(State::kPerformUpdate)
        .then([this]() {
            // Replace the cmd name to the equivalent cluster cmd name.
            auto updateCmdObj = _metadata.getUpdateCommand();
            auto cmdName = updateCmdObj.firstElement().fieldNameStringData();
            uassert(8126601,
                    str::stream() << "Unsupported cmd specified for multi update: " << cmdName,
                    (cmdName == "update"_sd) || (cmdName == "delete"_sd));

            auto modifiedCmdObj =
                cluster::cmd::translations::replaceCommandNameWithClusterCommandName(updateCmdObj);

            // Call the modified command.
            auto opCtxHolder = cc().makeOperationContext();
            auto opCtx = opCtxHolder.get();

            auto opMsgRequest =
                OpMsgRequestBuilder::create(_metadata.getNss().dbName(), modifiedCmdObj);
            auto requestMessage = opMsgRequest.serialize();

            return _externalState->sendClusterUpdateCommandToShards(opCtx, requestMessage);
        })
        .thenRunOn(**_taskExecutor)
        .then([this](DbResponse dbResponse) {
            _cmdResponse = rpc::makeReply(&dbResponse.response)->getCommandReply();
        });
}

ExecutorFuture<void> MultiUpdateCoordinatorInstance::_checkForPendingUpdates() {
    if (_cmdResponse || (_getCurrentState() > State::kPerformUpdate)) {
        return ExecutorFuture<void>(**_taskExecutor, Status::OK());
    }

    // TODO(SERVER-81267): $currentOp check for pending updates.
    return ExecutorFuture<void>(**_taskExecutor, Status::OK());
}

ExecutorFuture<void> MultiUpdateCoordinatorInstance::_cleanup() {
    if (_getCurrentState() > State::kCleanup) {
        return ExecutorFuture<void>(**_taskExecutor, Status::OK());
    }

    return _transitionToState(State::kCleanup);
}

void MultiUpdateCoordinatorInstance::_endOperation() {
    // TODO(SERVER-81265): chain call to endOperation().
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

ExecutorFuture<void> MultiUpdateCoordinatorInstance::_transitionToState(
    MultiUpdateCoordinatorStateEnum newState) {
    return _retry->untilStepdownOrMajorityCommit(
        fmt::format("transitionToState({})", MultiUpdateCoordinatorState_serializer(newState)),
        [this, newState](const auto& factory) {
            auto oldState = _getCurrentState();
            if (oldState >= newState) {
                return;
            }
            auto opCtx = factory.makeOperationContext(&cc());
            auto newDocument = _buildCurrentStateDocument();
            newDocument.getMutableFields().setState(newState);
            if (newState == State::kCleanup) {
                newDocument.getMutableFields().setResult(_cmdResponse);
            }
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
    if (oldState == State::kUnused) {
        store.add(opCtx, newStateDocument, WriteConcerns::kLocalWriteConcern);
    } else if (newState == State::kDone) {
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
