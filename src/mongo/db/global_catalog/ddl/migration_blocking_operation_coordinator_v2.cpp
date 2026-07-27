// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_catalog/ddl/migration_blocking_operation_coordinator_v2.h"

#include "mongo/logv2/log.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
MONGO_FAIL_POINT_DEFINE(hangBeforeBlockingMigrationsV2);
MONGO_FAIL_POINT_DEFINE(hangBeforeAllowingMigrationsV2);
MONGO_FAIL_POINT_DEFINE(failToPersistMigrationBlockingOperations);

namespace {
const Seconds kRequestQueuePollInterval{5};

const Status kCleaningUpStatus{
    ErrorCodes::MigrationBlockingOperationCoordinatorCleaningUp,
    "MigrationBlockingOperationCoordinatorV2 is cleaning up; retry the operation"};
}  // namespace

MigrationBlockingOperationCoordinatorV2::UUIDSet populateOperations(
    MigrationBlockingOperationCoordinatorV2Document doc) {
    auto operationsVector = doc.getOperations().get_value_or({});
    MigrationBlockingOperationCoordinatorV2::UUIDSet operationsSet;

    for (const auto& uuid : operationsVector) {
        tassert(13157101,
                str::stream() << "Duplicate operations found on disk with same UUID: " << uuid,
                !operationsSet.contains(uuid));
        operationsSet.insert(uuid);
    }

    if (!operationsSet.empty()) {
        tassert(13157102,
                "Operations should only exist while migrations are being blocked",
                doc.getPhase() ==
                    MigrationBlockingOperationCoordinatorV2PhaseEnum::kCurrentlyBlockingMigrations);
    }

    return operationsSet;
}

std::shared_ptr<MigrationBlockingOperationCoordinatorV2>
MigrationBlockingOperationCoordinatorV2::getOrCreate(OperationContext* opCtx,
                                                     const NamespaceString& nss) {
    auto coordinatorDoc = [&] {
        StateDoc doc;
        doc.setShardingCoordinatorMetadata(
            {{nss, CoordinatorTypeEnum::kMigrationBlockingOperationV2}});
        return doc.toBSON();
    }();

    auto service = ShardingCoordinatorService::getService(opCtx);
    return checked_pointer_cast<MigrationBlockingOperationCoordinatorV2>(
        service->getOrCreateInstance(opCtx, std::move(coordinatorDoc), FixedFCVRegion{opCtx}));
}

boost::optional<std::shared_ptr<MigrationBlockingOperationCoordinatorV2>>
MigrationBlockingOperationCoordinatorV2::get(OperationContext* opCtx, const NamespaceString& nss) {
    auto coordinatorId = [&] {
        ShardingCoordinatorId id{nss, CoordinatorTypeEnum::kMigrationBlockingOperationV2};
        return BSON("_id" << id.toBSON());
    }();
    auto service = ShardingCoordinatorService::getService(opCtx);
    auto [maybeInstance, _] = ShardingCoordinator::lookup(opCtx, service, coordinatorId);
    if (!maybeInstance) {
        return boost::none;
    }
    return checked_pointer_cast<MigrationBlockingOperationCoordinatorV2>(*maybeInstance);
}

MigrationBlockingOperationCoordinatorV2::MigrationBlockingOperationCoordinatorV2(
    ShardingCoordinatorService* service, const BSONObj& initialState)
    : RecoverableShardingDDLCoordinator(
          service, "MigrationBlockingOperationCoordinatorV2", initialState),
      _operations{populateOperations(_copyDoc())} {}

void MigrationBlockingOperationCoordinatorV2::checkIfOptionsConflict(
    const BSONObj& stateDoc) const {}

Future<void> MigrationBlockingOperationCoordinatorV2::beginOperation(OperationContext* opCtx,
                                                                     const UUID& operationUUID) {
    return _enqueue(opCtx, RequestType::kBeginOperation, operationUUID);
}

Future<void> MigrationBlockingOperationCoordinatorV2::endOperation(OperationContext* opCtx,
                                                                   const UUID& operationUUID) {
    return _enqueue(opCtx, RequestType::kEndOperation, operationUUID);
}

bool MigrationBlockingOperationCoordinatorV2::isInCriticalSection(Phase phase) const {
    return false;
}

void MigrationBlockingOperationCoordinatorV2::_onInterrupt(Status status) {
    auto workflow = _runImplWorkflow.get().value_or(SemiFuture<void>::makeReady().share());
    std::move(workflow)
        .thenRunOn(_service->getInstanceCleanupExecutor())
        .onCompletion(
            [this, anchor = shared_from_this(), status](Status) { _closeAndFailQueue(status); })
        .getAsync([](auto&&) {});
}

ExecutorFuture<void> MigrationBlockingOperationCoordinatorV2::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    auto workflow = ExecutorFuture<void>(**executor)
                        .then(_buildPhaseHandler(Phase::kStartBlockingMigrations,
                                                 [this, anchor = shared_from_this()](auto* opCtx) {
                                                     _startBlockingMigrations(opCtx);
                                                 }))
                        .then(_buildPhaseHandler(Phase::kCurrentlyBlockingMigrations,
                                                 [this, anchor = shared_from_this()](auto* opCtx) {
                                                     _processRequests(opCtx);
                                                 }))
                        .then(_buildPhaseHandler(Phase::kStopBlockingMigrations,
                                                 [this, anchor = shared_from_this()](auto* opCtx) {
                                                     _stopBlockingMigrations(opCtx);
                                                 }))
                        .semi()
                        .share();

    _runImplWorkflow = workflow;

    return workflow.thenRunOn(**executor);
}

Future<void> MigrationBlockingOperationCoordinatorV2::_enqueue(OperationContext* opCtx,
                                                               RequestType type,
                                                               const UUID& operationUUID) {
    try {
        getConstructionCompletionFuture().get(opCtx);
        auto [promise, future] = makePromiseFuture<void>();
        _queue.push(Request{type, operationUUID, std::move(promise)}, opCtx);
        return std::move(future);
    } catch (const ExceptionFor<ErrorCodes::ProducerConsumerQueueEndClosed>&) {
        // The producer end is closed once the coordinator begins tearing down.
        return kCleaningUpStatus;
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

void MigrationBlockingOperationCoordinatorV2::_startBlockingMigrations(OperationContext* opCtx) {
    hangBeforeBlockingMigrationsV2.pauseWhileSet();
    _getExternalState()->allowMigrations(
        opCtx,
        nss(),
        false,
        [&] { return getNewSession(opCtx); },
        _doc.getAuthoritativeMetadataAccessLevel());
    LOGV2(9554707, "MigrationBlockingOperationCoordinatorV2 started blocking migrations");
}

void MigrationBlockingOperationCoordinatorV2::_processRequests(OperationContext* opCtx) {
    while (true) {
        opCtx->checkForInterrupt();
        if (auto request = _popRequest(opCtx, kRequestQueuePollInterval)) {
            _handleRequest(opCtx, *request);
        }
        if (_operations.empty()) {
            break;
        }
    }
    // After we end the final operation and decide to clean up, it's possible that more requests
    // were received. We close the producer end of the queue and fail any remaining requests with a
    // retryable error, so that their callers retry on a new instance.
    _closeAndFailQueue(kCleaningUpStatus);
}

boost::optional<MigrationBlockingOperationCoordinatorV2::Request>
MigrationBlockingOperationCoordinatorV2::_popRequest(OperationContext* opCtx,
                                                     Milliseconds timeout) {
    try {
        const auto deadline = opCtx->getServiceContext()->getFastClockSource()->now() + timeout;
        return opCtx->runWithDeadline(
            deadline, ErrorCodes::ExceededTimeLimit, [&] { return _queue.pop(opCtx); });
    } catch (const ExceptionFor<ErrorCodes::ExceededTimeLimit>&) {
        return boost::none;
    }
}

void MigrationBlockingOperationCoordinatorV2::_stopBlockingMigrations(OperationContext* opCtx) {
    hangBeforeAllowingMigrationsV2.pauseWhileSet();
    _getExternalState()->allowMigrations(
        opCtx,
        nss(),
        true,
        [&] { return getNewSession(opCtx); },
        _doc.getAuthoritativeMetadataAccessLevel());
    LOGV2(9554708, "MigrationBlockingOperationCoordinatorV2 stopped blocking migrations");
}

void MigrationBlockingOperationCoordinatorV2::_handleRequest(OperationContext* opCtx,
                                                             Request& request) {
    try {
        _applyRequest(opCtx, request);
        request.result.emplaceValue();
    } catch (const DBException& ex) {
        request.result.setError(ex.toStatus());
    }
}

void MigrationBlockingOperationCoordinatorV2::_applyRequest(OperationContext* opCtx,
                                                            Request& request) {
    const bool modified = [&] {
        switch (request.type) {
            case RequestType::kBeginOperation:
                return _operations.insert(request.id).second;
            case RequestType::kEndOperation:
                return _operations.erase(request.id) > 0;
        }
        MONGO_UNREACHABLE_TASSERT(13157100);
    }();

    if (modified) {
        ScopeGuard rollback([&] {
            switch (request.type) {
                case RequestType::kBeginOperation:
                    _operations.erase(request.id);
                    break;
                case RequestType::kEndOperation:
                    _operations.insert(request.id);
                    break;
            }
        });
        _persistOperations(opCtx, _operations);
        rollback.dismiss();
    }
}

void MigrationBlockingOperationCoordinatorV2::_persistOperations(OperationContext* opCtx,
                                                                 const UUIDSet& operations) {
    uassert(ErrorCodes::InternalError,
            "Failing to persist operations because failToPersistMigrationBlockingOperations is set",
            !failToPersistMigrationBlockingOperations.shouldFail());

    auto newDoc = _copyDoc();
    newDoc.setOperations(std::vector<UUID>(operations.begin(), operations.end()));
    _updateStateDocument(opCtx, std::move(newDoc));
}

void MigrationBlockingOperationCoordinatorV2::_closeAndFailQueue(const Status& status) {
    _queue.closeProducerEnd();
    try {
        while (true) {
            _queue.pop().result.setError(status);
        }
    } catch (const ExceptionFor<ErrorCodes::ProducerConsumerQueueConsumed>&) {
        // The queue has been fully drained.
    }
}

}  // namespace mongo
