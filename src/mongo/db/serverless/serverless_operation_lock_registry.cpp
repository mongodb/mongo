/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/serverless/serverless_operation_lock_registry.h"

#include <boost/move/utility_core.hpp>
#include <mutex>
#include <string>
#include <utility>

#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/tenant_migration_state_machine_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTenantMigration

// Failpoint that will cause recoverLocks to return early.
MONGO_FAIL_POINT_DEFINE(skipRecoverServerlessOperationLock);
namespace mongo {

const ServiceContext::Decoration<ServerlessOperationLockRegistry>
    ServerlessOperationLockRegistry::get =
        ServiceContext::declareDecoration<ServerlessOperationLockRegistry>();

void ServerlessOperationLockRegistry::acquireLock(
    ServerlessOperationLockRegistry::LockType lockType, const UUID& operationId) {
    stdx::lock_guard<stdx::mutex> lg(_mutex);

    // Verify there is no serverless operation in progress or it is the same type as the one
    // acquiring the lock.
    uassert(ErrorCodes::ConflictingServerlessOperation,
            str::stream()
                << "Conflicting serverless operation in progress. Trying to acquire lock for '"
                << lockType << "' and operationId '" << operationId
                << "' but it is already used by '" << *_activeLockType << "' for operations "
                << printActiveOperations(lg),
            !_activeLockType || _activeLockType.get() == lockType);
    invariant(_activeOperations.find(operationId) == _activeOperations.end(),
              "Cannot acquire the serverless lock twice for the same operationId.");
    _activeLockType = lockType;

    _activeOperations.emplace(operationId);

    LOGV2(6531500,
          "Acquired serverless operation lock",
          "type"_attr = lockType,
          "id"_attr = operationId);
}

void ServerlessOperationLockRegistry::releaseLock(
    ServerlessOperationLockRegistry::LockType lockType, const UUID& operationId) {
    stdx::lock_guard<stdx::mutex> lg(_mutex);

    invariant(_activeLockType && *_activeLockType == lockType,
              "Cannot release a serverless lock that is not owned by the given lock type.");

    invariant(_activeOperations.find(operationId) != _activeOperations.end(),
              "Cannot release a serverless lock if the given operationId does not own the lock.");
    _activeOperations.erase(operationId);

    if (_activeOperations.empty()) {
        _activeLockType.reset();
    }

    LOGV2(6531501,
          "Released serverless operation lock",
          "type"_attr = lockType,
          "id"_attr = operationId);
}

void ServerlessOperationLockRegistry::onDropStateCollection(LockType lockType) {
    stdx::lock_guard<stdx::mutex> lg(_mutex);

    if (!_activeLockType || *_activeLockType != lockType) {
        return;
    }

    LOGV2(6531505,
          "Released all serverless locks due to state collection drop",
          "type"_attr = lockType);

    _activeLockType.reset();
    _activeOperations.clear();
}

void ServerlessOperationLockRegistry::clear() {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    LOGV2(6531504,
          "Clearing serverless operation lock registry on shutdown",
          "ns"_attr = _activeLockType);

    _activeOperations.clear();
    _activeLockType.reset();
}

void ServerlessOperationLockRegistry::recoverLocks(OperationContext* opCtx) {
    if (skipRecoverServerlessOperationLock.shouldFail()) {
        return;
    }

    auto& registry = ServerlessOperationLockRegistry::get(opCtx->getServiceContext());
    registry.clear();

    PersistentTaskStore<TenantMigrationDonorDocument> donorStore(
        NamespaceString::kTenantMigrationDonorsNamespace);
    donorStore.forEach(opCtx, {}, [&](const TenantMigrationDonorDocument& doc) {
        // Do not acquire a lock for garbage-collectable documents.
        if (doc.getExpireAt()) {
            return true;
        }

        registry.acquireLock(ServerlessOperationLockRegistry::LockType::kTenantDonor, doc.getId());

        return true;
    });

    PersistentTaskStore<TenantMigrationRecipientDocument> recipientStore(
        NamespaceString::kTenantMigrationRecipientsNamespace);
    recipientStore.forEach(opCtx, {}, [&](const TenantMigrationRecipientDocument& doc) {
        // Do not acquire a lock for garbage-collectable documents.
        if (doc.getExpireAt()) {
            return true;
        }

        registry.acquireLock(ServerlessOperationLockRegistry::LockType::kTenantRecipient,
                             doc.getId());

        return true;
    });
}

const std::string kOperationLockFieldName = "operationLock";
void ServerlessOperationLockRegistry::appendInfoForServerStatus(BSONObjBuilder* builder) const {
    stdx::lock_guard<stdx::mutex> lg(_mutex);

    if (!_activeLockType) {
        builder->append(kOperationLockFieldName, 0);
        return;
    }

    switch (_activeLockType.value()) {
        case ServerlessOperationLockRegistry::LockType::kTenantDonor:
            builder->append(kOperationLockFieldName, 2);
            break;
        case ServerlessOperationLockRegistry::LockType::kTenantRecipient:
            builder->append(kOperationLockFieldName, 3);
            break;
        case ServerlessOperationLockRegistry::LockType::kMergeRecipient:
            builder->append(kOperationLockFieldName, 4);
            break;
    }
}

boost::optional<ServerlessOperationLockRegistry::LockType>
ServerlessOperationLockRegistry::getActiveOperationType_forTest() {
    stdx::lock_guard<stdx::mutex> lg(_mutex);

    return _activeLockType;
}

std::string ServerlessOperationLockRegistry::printActiveOperations(WithLock lock) const {
    StringBuilder sb;
    sb << "[";
    for (const auto& uuid : _activeOperations) {
        sb << uuid << ",";
    }
    sb << "]";

    return sb.str();
}

}  // namespace mongo
