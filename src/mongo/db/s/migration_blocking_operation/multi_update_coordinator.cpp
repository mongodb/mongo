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
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/json.h"
#include "mongo/db/cluster_command_translations.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/s/migration_blocking_operation/multi_update_coordinator_server_parameters_gen.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/primary_only_service_helpers/pause_during_phase_transition_fail_point.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/rpc/protocol.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/util/duration.h"
#include "mongo/util/future_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {
MONGO_FAIL_POINT_DEFINE(pauseDuringMultiUpdateCoordinatorPhaseTransition);
MONGO_FAIL_POINT_DEFINE(pauseDuringMultiUpdateCoordinatorPhaseTransitionAlternate);
MONGO_FAIL_POINT_DEFINE(hangDuringMultiUpdateCoordinatorRun);
MONGO_FAIL_POINT_DEFINE(hangDuringMultiUpdateCoordinatorPendingUpdates);
MONGO_FAIL_POINT_DEFINE(hangAfterMultiUpdateCoordinatorSendsUpdates);
MONGO_FAIL_POINT_DEFINE(hangAfterAbortingMultiUpdateCoordinators);
using Phase = MultiUpdateCoordinatorPhaseEnum;

primary_only_service_helpers::PauseDuringPhaseTransitionFailPoint<MultiUpdateCoordinatorPhaseEnum>
    pauseDuringPhaseTransitions{
        {pauseDuringMultiUpdateCoordinatorPhaseTransition,
         pauseDuringMultiUpdateCoordinatorPhaseTransitionAlternate},
        [](StringData phase) {
            IDLParserContext ectx(
                "pauseDuringMultiUpdateCoordinatorPhaseTransition::readPhaseArgument");
            return MultiUpdateCoordinatorPhase_parse(ectx, phase);
        }};

AggregateCommandRequest makeAggregationToCheckForPendingUpdates(const NamespaceString& nss,
                                                                const LogicalSessionId& lsid) {
    auto currentOpStage = fromjson("{$currentOp: {allUsers: true, idleSessions: true}}");
    auto matchStage = BSON("$match" << BSON("type"
                                            << "op"
                                            << "op"
                                            << "command"
                                            << "lsid" << lsid.toBSON()));

    AggregateCommandRequest request{nss};
    request.setPipeline({currentOpStage, matchStage});
    return request;
}

auto getPhaseTransitionStopEvent(MultiUpdateCoordinatorPhaseEnum phase) {
    using Event = primary_only_service_helpers::RetryUntilMajorityCommit::Event;
    switch (phase) {
        case Phase::kSuccess:
        case Phase::kFailure:
        case Phase::kDone:
            return Event::kStepdown;
        default:
            return Event::kAbort;
    }
    MONGO_UNREACHABLE;
}

}  // namespace

void MultiUpdateCoordinatorService::abortAndWaitForAllInstances(OperationContext* opCtx,
                                                                Status reason) {
    auto service = [&] {
        auto registry = repl::PrimaryOnlyServiceRegistry::get(opCtx->getServiceContext());
        const auto& name = MultiUpdateCoordinatorService::kServiceName;
        return checked_cast<MultiUpdateCoordinatorService*>(registry->lookupServiceByName(name));
    }();
    auto instances = [&] {
        auto untyped = service->getAllInstances(opCtx);
        std::vector<std::shared_ptr<MultiUpdateCoordinatorInstance>> typed;
        for (const auto& instance : untyped) {
            typed.emplace_back(checked_pointer_cast<MultiUpdateCoordinatorInstance>(instance));
        }
        return typed;
    }();
    for (const auto& instance : instances) {
        instance->abort(reason);
    }
    hangAfterAbortingMultiUpdateCoordinators.pauseWhileSet();
    for (const auto& instance : instances) {
        instance->getCompletionFuture().wait();
    }
}

MultiUpdateCoordinatorService::MultiUpdateCoordinatorService(ServiceContext* serviceContext)
    : MultiUpdateCoordinatorService{
          serviceContext,
          std::make_unique<MultiUpdateCoordinatorExternalStateFactoryImpl>(serviceContext)} {}

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
      _beganInPhase{_mutableFields.getPhase()},
      _externalState{service->_externalStateFactory->createExternalState()},
      _cmdResponse{_mutableFields.getResult()},
      _abortReason{_mutableFields.getAbortReason()} {}

SharedSemiFuture<BSONObj> MultiUpdateCoordinatorInstance::getCompletionFuture() const {
    return _completionPromise.getFuture();
}

void MultiUpdateCoordinatorInstance::abort(Status reason) {
    invariant(!reason.isOK());
    stdx::lock_guard lock(_mutex);
    if (_abortReason) {
        return;
    }
    LOGV2(8127500, "Sending MultiUpdateCoordinator signal to abort", "reason"_attr = reason);
    _abortReason = std::move(reason);
    _cancelState->abort();
}

SemiFuture<void> MultiUpdateCoordinatorInstance::run(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& stepdownToken) noexcept {
    _initializeRun(executor, stepdownToken);

    hangDuringMultiUpdateCoordinatorRun.pauseWhileSet();

    _completionPromise.setFrom(
        _runWorkflow().unsafeToInlineFuture().tapError([](const Status& status) {
            LOGV2(8514201,
                  "MultiUpdateCoordinator encountered an error",
                  "error"_attr = redact(status));
        }));

    return getCompletionFuture().semi().ignoreValue();
}

ExecutorFuture<BSONObj> MultiUpdateCoordinatorInstance::_runWorkflow() {
    return ExecutorFuture(**_taskExecutor)
        .then([this] { return _transitionToPhase(Phase::kAcquireSession); })
        .then([this] { return _doAcquireSessionPhase(); })
        .then([this] { return _transitionToPhase(Phase::kBlockMigrations); })
        .then([this] { return _doBlockMigrationsPhase(); })
        .then([this] { return _transitionToPhase(Phase::kPerformUpdate); })
        .then([this] { return _doPerformUpdatePhase(); })
        .onCompletion([this](Status status) {
            if (status.isOK()) {
                invariant(_cmdResponse);
                return _transitionToPhase(Phase::kSuccess);
            }
            abort(status);
            return _transitionToPhase(Phase::kFailure);
        })
        .thenRunOn(_service->getInstanceCleanupExecutor())
        .then([this] { return _stopBlockingMigrationsIfNeeded(); })
        .then([this] { return _transitionToPhase(Phase::kDone); })
        .onCompletion([this, self = shared_from_this()](Status okOrStepdownError) {
            if (_shouldReleaseSession()) {
                _releaseSession();
            }
            const auto steppingDown = _currentlySteppingDown();
            invariant(okOrStepdownError.isOK() || steppingDown);
            return steppingDown ? okOrStepdownError : _getResult();
        });
}

ExecutorFuture<void> MultiUpdateCoordinatorInstance::_doAcquireSessionPhase() {
    return _retry->untilAbortOrSuccess("_doAcquireSessionPhase()", [this](const auto& factory) {
        if (_getCurrentPhase() > Phase::kAcquireSession) {
            return;
        }
        _acquireSession();
    });
}

ExecutorFuture<void> MultiUpdateCoordinatorInstance::_doBlockMigrationsPhase() {
    return _retry->untilAbortOrSuccess("_doBlockMigrationsPhase", [this](const auto& factory) {
        if (_getCurrentPhase() > Phase::kBlockMigrations) {
            return;
        }
        auto opCtx = factory.makeOperationContext(&cc());
        _externalState->startBlockingMigrations(opCtx.get(), _metadata.getNss(), _metadata.getId());
    });
}

ExecutorFuture<void> MultiUpdateCoordinatorInstance::_doPerformUpdatePhase() {
    if (_getCurrentPhase() > Phase::kPerformUpdate) {
        return ExecutorFuture(**_taskExecutor);
    }

    if (_updatesPossiblyRunningFromPreviousTerm()) {
        return _waitForPendingUpdates();
    }
    return _sendUpdateRequest();
}

Message MultiUpdateCoordinatorInstance::getUpdateAsClusterCommand() const {
    auto updateCmdObj = _metadata.getUpdateCommand();
    auto cmdName = updateCmdObj.firstElement().fieldNameStringData();
    uassert(8126601,
            str::stream() << "Unsupported cmd specified for multi update: " << cmdName,
            (cmdName == "update"_sd) || (cmdName == "delete"_sd));

    auto modifiedCmdObj =
        cluster::cmd::translations::replaceCommandNameWithClusterCommandName(updateCmdObj);
    auto opMsgRequest = OpMsgRequestBuilder::create(
        auth::ValidatedTenancyScope::kNotRequired, _metadata.getNss().dbName(), modifiedCmdObj);
    return opMsgRequest.serialize();
}

ExecutorFuture<void> MultiUpdateCoordinatorInstance::_sendUpdateRequest() {
    return _retry->untilAbortOrSuccess("_sendUpdateRequest", [this](const auto& factory) {
        auto opCtx = factory.makeOperationContext(&cc());
        {
            auto lk = stdx::lock_guard(*opCtx->getClient());
            opCtx->setLogicalSessionId(_getSessionId());
        }
        auto futureResponse = _externalState->sendClusterUpdateCommandToShards(
            opCtx.get(), getUpdateAsClusterCommand());
        hangAfterMultiUpdateCoordinatorSendsUpdates.pauseWhileSet();
        return future_util::withCancellation(std::move(futureResponse),
                                             _cancelState->getAbortOrStepdownToken())
            .thenRunOn(**_taskExecutor)
            .then([this](DbResponse dbResponse) {
                _cmdResponse = rpc::makeReply(&dbResponse.response)->getCommandReply();
            });
    });
}

ExecutorFuture<void> MultiUpdateCoordinatorInstance::_waitForPendingUpdates() {
    return _retry
        ->untilAbortOrSuccess(
            "_waitForPendingUpdates",
            [this](const auto& factory) {
                auto opCtx = factory.makeOperationContext(&cc());
                auto nss = NamespaceString::makeCollectionlessAggregateNSS(DatabaseName::kAdmin);
                auto request = makeAggregationToCheckForPendingUpdates(nss, _getSessionId());
                auto updatesPending = _externalState->isUpdatePending(opCtx.get(), nss, request);
                if (updatesPending) {
                    hangDuringMultiUpdateCoordinatorPendingUpdates.pauseWhileSet();
                    uasserted(ErrorCodes::UpdatesStillPending, "Updates still pending");
                }
            })
        .onCompletion([this](const Status& result) {
            uasserted(8126701,
                      "Encountered a failover while executing multi update/delete operation.");
        });
}

ExecutorFuture<void> MultiUpdateCoordinatorInstance::_stopBlockingMigrationsIfNeeded() {
    return _retry->untilStepdownOrSuccess(
        "MultiUpdateCoordinator::stopBlockingMigration", [this](const auto& factory) {
            if (!_shouldUnblockMigrations()) {
                return;
            }
            auto opCtx = factory.makeOperationContext(&cc());
            return _externalState->stopBlockingMigrations(
                opCtx.get(), _metadata.getNss(), _metadata.getId());
        });
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

const boost::optional<Status>& MultiUpdateCoordinatorInstance::_getAbortReason() const {
    stdx::lock_guard lock(_mutex);
    return _abortReason;
}

StatusWith<BSONObj> MultiUpdateCoordinatorInstance::_getResult() const {
    if (_cmdResponse) {
        return *_cmdResponse;
    }
    const auto& abortReason = _getAbortReason();
    invariant(abortReason);
    return *abortReason;
}

MultiUpdateCoordinatorMutableFields MultiUpdateCoordinatorInstance::_getMutableFields() const {
    stdx::unique_lock lock(_mutex);
    return _mutableFields;
}

MultiUpdateCoordinatorPhaseEnum MultiUpdateCoordinatorInstance::_getCurrentPhase() const {
    return _getMutableFields().getPhase();
}

MultiUpdateCoordinatorDocument MultiUpdateCoordinatorInstance::_buildCurrentStateDocument() const {
    MultiUpdateCoordinatorDocument document;
    document.setMetadata(getMetadata());
    document.setMutableFields(_getMutableFields());
    return document;
}

void MultiUpdateCoordinatorInstance::_acquireSession() {
    auto session = _externalState->acquireSession();
    stdx::unique_lock lock(_mutex);
    _mutableFields.setLsid(session.getSessionId());
    _mutableFields.setTxnNumber(session.getTxnNumber());
}

void MultiUpdateCoordinatorInstance::_releaseSession() {
    stdx::unique_lock lock(_mutex);
    _externalState->releaseSession({*_mutableFields.getLsid(), *_mutableFields.getTxnNumber()});
}

const LogicalSessionId& MultiUpdateCoordinatorInstance::_getSessionId() const {
    stdx::unique_lock lock(_mutex);
    return *_mutableFields.getLsid();
}

bool MultiUpdateCoordinatorInstance::_sessionIsPersisted() const {
    return _getCurrentPhase() > Phase::kAcquireSession;
}

bool MultiUpdateCoordinatorInstance::_sessionIsCheckedOut() const {
    auto fields = _getMutableFields();
    return fields.getLsid().has_value() && fields.getTxnNumber().has_value();
}

bool MultiUpdateCoordinatorInstance::_shouldReleaseSession() const {
    if (!_sessionIsCheckedOut()) {
        return false;
    }
    return !_sessionIsPersisted() || _getCurrentPhase() >= Phase::kDone;
}

bool MultiUpdateCoordinatorInstance::_shouldUnblockMigrations() const {
    return _getCurrentPhase() > Phase::kBlockMigrations;
}

bool MultiUpdateCoordinatorInstance::_updatesPossiblyRunningFromPreviousTerm() const {
    return _beganInPhase == Phase::kPerformUpdate;
}

bool MultiUpdateCoordinatorInstance::_currentlySteppingDown() const {
    return _cancelState->isSteppingDown() || (**_taskExecutor)->isShuttingDown();
}

void MultiUpdateCoordinatorInstance::_initializeRun(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& stepdownToken) {
    _taskExecutor = executor;
    _cancelState.emplace(stepdownToken);
    if (_getAbortReason()) {
        _cancelState->abort();
    }
    _retry.emplace(
        _service->getServiceName(), _taskExecutor, _cancelState.get_ptr(), getMetadata().toBSON());
}

ExecutorFuture<void> MultiUpdateCoordinatorInstance::_transitionToPhase(
    MultiUpdateCoordinatorPhaseEnum newPhase) {
    return _retry->untilMajorityCommitOr(
        getPhaseTransitionStopEvent(newPhase),
        fmt::format("_transitionToPhase({})", MultiUpdateCoordinatorPhase_serializer(newPhase)),
        [this, newPhase](const auto& factory) {
            auto oldPhase = _getCurrentPhase();
            if (oldPhase >= newPhase) {
                return;
            }
            auto opCtx = factory.makeOperationContext(&cc());
            auto newDocument = _buildCurrentStateDocument();
            newDocument.getMutableFields().setPhase(newPhase);
            if (newPhase == Phase::kSuccess) {
                newDocument.getMutableFields().setResult(_cmdResponse);
            } else if (newPhase == Phase::kFailure) {
                newDocument.getMutableFields().setAbortReason(_abortReason);
            }
            pauseDuringPhaseTransitions.evaluate(PhaseTransitionProgressEnum::kBefore, newPhase);
            _updateOnDiskState(opCtx.get(), newDocument);
            pauseDuringPhaseTransitions.evaluate(PhaseTransitionProgressEnum::kPartial, newPhase);
            _updateInMemoryState(newDocument);

            LOGV2(8514200,
                  "MultiUpdateCoordinator transitioned phase",
                  "oldPhase"_attr = MultiUpdateCoordinatorPhase_serializer(oldPhase),
                  "newPhase"_attr = MultiUpdateCoordinatorPhase_serializer(newPhase),
                  "id"_attr = _metadata.getId(),
                  "namespace"_attr = _metadata.getNss());

            pauseDuringPhaseTransitions.evaluate(PhaseTransitionProgressEnum::kAfter, newPhase);
        });
}

void MultiUpdateCoordinatorInstance::_updateOnDiskState(
    OperationContext* opCtx, const MultiUpdateCoordinatorDocument& newPhaseDocument) {
    PersistentTaskStore<MultiUpdateCoordinatorDocument> store(_service->getStateDocumentsNS());
    auto oldPhase = _getCurrentPhase();
    auto newPhase = newPhaseDocument.getMutableFields().getPhase();
    if (oldPhase == Phase::kUnused) {
        store.add(opCtx, newPhaseDocument, WriteConcerns::kLocalWriteConcern);
    } else if (newPhase == Phase::kDone) {
        store.remove(opCtx,
                     BSON(MultiUpdateCoordinatorDocument::kIdFieldName << _metadata.getId()),
                     WriteConcerns::kLocalWriteConcern);
    } else {
        store.update(opCtx,
                     BSON(MultiUpdateCoordinatorDocument::kIdFieldName << _metadata.getId()),
                     BSON("$set" << BSON(MultiUpdateCoordinatorDocument::kMutableFieldsFieldName
                                         << newPhaseDocument.getMutableFields().toBSON())),
                     WriteConcerns::kLocalWriteConcern);
    }
}

void MultiUpdateCoordinatorInstance::_updateInMemoryState(
    const MultiUpdateCoordinatorDocument& newPhaseDocument) {
    stdx::unique_lock lock(_mutex);
    _mutableFields = newPhaseDocument.getMutableFields();
}

}  // namespace mongo
