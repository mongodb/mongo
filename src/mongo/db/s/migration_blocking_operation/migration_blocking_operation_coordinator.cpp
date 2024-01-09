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

#include "mongo/db/s/migration_blocking_operation/migration_blocking_operation_coordinator.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

MigrationBlockingOperationCoordinator::UUIDSet recoverOperations(
    MigrationBlockingOperationCoordinatorDocument doc) {
    auto operationsVector = doc.getOperations().get_value_or({});
    if (!operationsVector.empty()) {
        invariant(doc.getPhase() ==
                      MigrationBlockingOperationCoordinatorPhaseEnum::kBlockingMigrations,
                  str::stream() << "Operations should not be ongoing while migrations are running");
    }

    MigrationBlockingOperationCoordinator::UUIDSet operationsSet = {};
    for (const auto& uuid : operationsVector) {
        invariant(!operationsSet.contains(uuid),
                  str::stream() << "Duplicate operations found on disk with same UUID: " << uuid);
        operationsSet.insert(uuid);
    }
    return operationsSet;
}

MigrationBlockingOperationCoordinator::MigrationBlockingOperationCoordinator(
    ShardingDDLCoordinatorService* service, const BSONObj& initialState)
    : RecoverableShardingDDLCoordinator(
          service, "MigrationBlockingOperationCoordinator", initialState),
      _operations{recoverOperations(_getDoc())} {}

void MigrationBlockingOperationCoordinator::checkIfOptionsConflict(const BSONObj& stateDoc) const {}

StringData MigrationBlockingOperationCoordinator::serializePhase(const Phase& phase) const {
    return MigrationBlockingOperationCoordinatorPhase_serializer(phase);
}

ExecutorFuture<void> MigrationBlockingOperationCoordinator::_runImpl(
    std::shared_ptr<executor::ScopedTaskExecutor> executor,
    const CancellationToken& token) noexcept {
    return _completionPromise.getFuture().thenRunOn(**executor);
}

MigrationBlockingOperationCoordinatorPhaseEnum
MigrationBlockingOperationCoordinator::_getCurrentPhase() const {
    stdx::unique_lock lock(_docMutex);
    return _doc.getPhase();
}

bool MigrationBlockingOperationCoordinator::_isFirstOperation(WithLock) const {
    return _operations.size() == 1 && _getCurrentPhase() != Phase::kBlockingMigrations;
}

void MigrationBlockingOperationCoordinator::_throwIfCleaningUp() {
    uassert(
        ErrorCodes::MigrationBlockingOperationCoordinatorCleaningUp,
        str::stream() << "Migration blocking operation coordinator is currently being cleaned up",
        !_completionPromise.getFuture().isReady());
}

void MigrationBlockingOperationCoordinator::beginOperation(OperationContext* opCtx,
                                                           const UUID& operationUUID) {
    stdx::unique_lock lock(_mutex);
    _throwIfCleaningUp();

    if (_operations.contains(operationUUID)) {
        return;
    }
    _operations.insert(operationUUID);
    ScopeGuard removeOperationGuard([&] { _operations.erase(operationUUID); });

    auto newDoc = _getDoc();
    auto isFirst = _isFirstOperation(lock);

    if (isFirst) {
        newDoc.setOperations(std::vector<UUID>{});
        newDoc.setPhase(Phase::kBlockingMigrations);
    }
    newDoc.getOperations()->push_back(operationUUID);

    _insertOrUpdateStateDocument(lock, opCtx, std::move(newDoc));

    if (isFirst) {
        ScopeGuard removeStateDocumentGuard([&] { _completionPromise.emplaceValue(); });

        _getExternalState()->allowMigrations(opCtx, nss(), false);
        removeStateDocumentGuard.dismiss();
    }

    removeOperationGuard.dismiss();
}

void MigrationBlockingOperationCoordinator::endOperation(OperationContext* opCtx,
                                                         const UUID& operationUUID) {
    stdx::unique_lock lock(_mutex);
    _throwIfCleaningUp();

    if (!_operations.contains(operationUUID)) {
        return;
    }
    _operations.erase(operationUUID);

    if (_operations.empty()) {
        ScopeGuard insertOperationGuard([&] { _operations.insert(operationUUID); });

        _getExternalState()->allowMigrations(opCtx, nss(), true);
        insertOperationGuard.dismiss();

        _completionPromise.emplaceValue();
        return;
    }

    auto newDoc = _getDoc();
    std::erase(newDoc.getOperations().get(), operationUUID);

    _insertOrUpdateStateDocument(lock, opCtx, std::move(newDoc));
}

void MigrationBlockingOperationCoordinator::_insertOrUpdateStateDocument(
    WithLock lk,
    OperationContext* opCtx,
    MigrationBlockingOperationCoordinatorDocument newStateDocument) {
    if (_isFirstOperation(lk)) {
        _insertStateDocument(opCtx, std::move(newStateDocument));
    } else {
        _updateStateDocument(opCtx, std::move(newStateDocument));
    }
}

}  // namespace mongo
