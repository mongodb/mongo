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

#include "mongo/db/global_catalog/ddl/migration_blocking_operation_coordinator.h"

#include "mongo/util/future_util.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
MONGO_FAIL_POINT_DEFINE(hangBeforeUpdatingInMemory);
MONGO_FAIL_POINT_DEFINE(hangBeforeUpdatingDiskState);
MONGO_FAIL_POINT_DEFINE(hangBeforeBlockingMigrations);
MONGO_FAIL_POINT_DEFINE(hangBeforeAllowingMigrations);
MONGO_FAIL_POINT_DEFINE(hangBeforeFulfillingPromise);

MigrationBlockingOperationCoordinator::UUIDSet populateOperations(
    MigrationBlockingOperationCoordinatorDocument doc) {
    auto operationsVector = doc.getOperations().get_value_or({});
    MigrationBlockingOperationCoordinator::UUIDSet operationsSet;

    if (operationsVector.empty()) {
        return operationsSet;
    }
    tassert(10644533,
            "Operations should not be ongoing while migrations are running",
            doc.getPhase() == MigrationBlockingOperationCoordinatorPhaseEnum::kBlockingMigrations);

    for (const auto& uuid : operationsVector) {
        tassert(10644534,
                str::stream() << "Duplicate operations found on disk with same UUID: " << uuid,
                !operationsSet.contains(uuid));
        operationsSet.insert(uuid);
    }
    return operationsSet;
}

void ensureFulfilledPromise(WithLock lk, SharedPromise<void>& sp) {
    if (!sp.getFuture().isReady()) {
        sp.emplaceValue();
    }
}

std::shared_ptr<MigrationBlockingOperationCoordinator>
MigrationBlockingOperationCoordinator::getOrCreate(OperationContext* opCtx,
                                                   const NamespaceString& nss) {
    auto coordinatorDoc = [&] {
        StateDoc doc;
        doc.setShardingDDLCoordinatorMetadata(
            {{nss, DDLCoordinatorTypeEnum::kMigrationBlockingOperation}});
        return doc.toBSON();
    }();

    auto service = ShardingDDLCoordinatorService::getService(opCtx);
    return checked_pointer_cast<MigrationBlockingOperationCoordinator>(
        service->getOrCreateInstance(opCtx, std::move(coordinatorDoc), FixedFCVRegion{opCtx}));
}

boost::optional<std::shared_ptr<MigrationBlockingOperationCoordinator>>
MigrationBlockingOperationCoordinator::get(OperationContext* opCtx, const NamespaceString& nss) {
    auto coordinatorId = [&] {
        ShardingDDLCoordinatorId id{nss, DDLCoordinatorTypeEnum::kMigrationBlockingOperation};
        return BSON("_id" << id.toBSON());
    }();
    auto service = ShardingDDLCoordinatorService::getService(opCtx);
    auto [maybeInstance, _] = ShardingDDLCoordinator::lookup(opCtx, service, coordinatorId);
    if (!maybeInstance) {
        return boost::none;
    }
    return checked_pointer_cast<MigrationBlockingOperationCoordinator>(*maybeInstance);
}

MigrationBlockingOperationCoordinator::MigrationBlockingOperationCoordinator(
    ShardingDDLCoordinatorService* service, const BSONObj& initialState)
    : RecoverableShardingDDLCoordinator(
          service, "MigrationBlockingOperationCoordinator", initialState),
      _operations{populateOperations(_getDoc())},
      _needsRecovery{_recoveredFromDisk} {}

void MigrationBlockingOperationCoordinator::checkIfOptionsConflict(const BSONObj& stateDoc) const {}

StringData MigrationBlockingOperationCoordinator::serializePhase(const Phase& phase) const {
    return MigrationBlockingOperationCoordinatorPhase_serializer(phase);
}

ExecutorFuture<void> MigrationBlockingOperationCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return future_util::withCancellation(_beginCleanupPromise.getFuture(), token)
        .thenRunOn(**executor);
}

MigrationBlockingOperationCoordinatorPhaseEnum
MigrationBlockingOperationCoordinator::_getCurrentPhase() const {
    stdx::unique_lock lock(_docMutex);
    return _doc.getPhase();
}

bool MigrationBlockingOperationCoordinator::_isFirstOperation(WithLock) const {
    return _operations.size() == 1 && _getCurrentPhase() != Phase::kBlockingMigrations;
}

void MigrationBlockingOperationCoordinator::_throwIfCleaningUp(WithLock) {
    uassert(
        ErrorCodes::MigrationBlockingOperationCoordinatorCleaningUp,
        str::stream() << "Migration blocking operation coordinator is currently being cleaned up",
        !_beginCleanupPromise.getFuture().isReady());
}

void MigrationBlockingOperationCoordinator::_recoverIfNecessary(WithLock lk,
                                                                OperationContext* opCtx,
                                                                bool isBeginOperation) {
    if (!_needsRecovery || !_getExternalState()->checkAllowMigrations(opCtx, nss())) {
        _needsRecovery = false;
        return;
    }

    tassert(10644530,
            "If there is a state document on disk and migrations are not "
            "blocked, then there must be only one operation.",
            _operations.size() == 1);

    if (isBeginOperation) {
        try {
            _getExternalState()->allowMigrations(opCtx, nss(), false);
            _needsRecovery = false;
            return;
        } catch (const DBException& e) {
            LOGV2(8127201,
                  "Error blocking migrations, starting instance clean up",
                  "error"_attr = e.toString());
        }
    }
    _operations.clear();
    ensureFulfilledPromise(lk, _beginCleanupPromise);
    getCompletionFuture().get();

    _needsRecovery = false;
}

void MigrationBlockingOperationCoordinator::beginOperation(OperationContext* opCtx,
                                                           const UUID& operationUUID) {
    getConstructionCompletionFuture().get();

    stdx::unique_lock lock(_mutex);

    _throwIfCleaningUp(lock);
    _recoverIfNecessary(lock, opCtx, true);

    if (_operations.contains(operationUUID)) {
        LOGV2(9554700,
              "MigrationBlockingOperationCoordinator ignoring request to begin operation with the "
              "same UUID as existing operation",
              "operationId"_attr = operationUUID,
              "operationCount"_attr = _operations.size());
        return;
    }

    hangBeforeUpdatingInMemory.pauseWhileSet();
    _operations.insert(operationUUID);
    ScopeGuard removeOperationGuard([&] { _operations.erase(operationUUID); });

    auto newDoc = _getDoc();
    auto isFirst = _isFirstOperation(lock);

    if (isFirst) {
        newDoc.setOperations(std::vector<UUID>{});
        newDoc.setPhase(Phase::kBlockingMigrations);
    }
    newDoc.getOperations()->push_back(operationUUID);

    hangBeforeUpdatingDiskState.pauseWhileSet();
    _insertOrUpdateStateDocument(lock, opCtx, std::move(newDoc));

    if (isFirst) {
        ScopeGuard removeStateDocumentGuard(
            [&] { ensureFulfilledPromise(lock, _beginCleanupPromise); });
        hangBeforeBlockingMigrations.pauseWhileSet();
        _getExternalState()->allowMigrations(opCtx, nss(), false);
        removeStateDocumentGuard.dismiss();
    }

    removeOperationGuard.dismiss();

    LOGV2(9554701,
          "MigrationBlockingOperationCoordinator began new operation",
          "operationId"_attr = operationUUID,
          "operationCount"_attr = _operations.size());
}

void MigrationBlockingOperationCoordinator::endOperation(OperationContext* opCtx,
                                                         const UUID& operationUUID) {
    getConstructionCompletionFuture().get();

    stdx::unique_lock lock(_mutex);

    _recoverIfNecessary(lock, opCtx, false);

    if (!_operations.contains(operationUUID)) {
        LOGV2(9554702,
              "MigrationBlockingOperationCoordinator ignoring request to end operation that does "
              "not exist",
              "operationId"_attr = operationUUID,
              "operationCount"_attr = _operations.size());
        return;
    }

    hangBeforeUpdatingInMemory.pauseWhileSet();
    _operations.erase(operationUUID);
    ScopeGuard insertOperationGuard([&] { _operations.insert(operationUUID); });

    if (_operations.empty()) {
        hangBeforeAllowingMigrations.pauseWhileSet();
        _getExternalState()->allowMigrations(opCtx, nss(), true);

        hangBeforeFulfillingPromise.pauseWhileSet();
        ensureFulfilledPromise(lock, _beginCleanupPromise);
        getCompletionFuture().get();
        insertOperationGuard.dismiss();

        LOGV2(9554703,
              "MigrationBlockingOperationCoordinator ended operation and has cleaned up",
              "operationId"_attr = operationUUID,
              "operationCount"_attr = _operations.size());
        return;
    }

    hangBeforeUpdatingDiskState.pauseWhileSet();
    auto newDoc = _getDoc();
    std::erase(newDoc.getOperations().get(), operationUUID);

    _insertOrUpdateStateDocument(lock, opCtx, std::move(newDoc));
    insertOperationGuard.dismiss();

    LOGV2(9554704,
          "MigrationBlockingOperationCoordinator ended operation",
          "operationId"_attr = operationUUID,
          "operationCount"_attr = _operations.size());
}

void MigrationBlockingOperationCoordinator::_insertOrUpdateStateDocument(
    WithLock lk, OperationContext* opCtx, StateDoc newStateDocument) {
    if (_isFirstOperation(lk)) {
        _insertStateDocument(opCtx, std::move(newStateDocument));
    } else {
        _updateStateDocument(opCtx, std::move(newStateDocument));
    }
}

}  // namespace mongo
