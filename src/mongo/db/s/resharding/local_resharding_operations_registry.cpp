/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/s/resharding/local_resharding_operations_registry.h"

#include "mongo/db/persistent_task_store.h"
#include "mongo/db/s/resharding/resharding_metrics_helpers.h"
#include "mongo/db/s/resharding/resharding_util.h"
#include "mongo/util/assert_util.h"

namespace mongo {

namespace {
const auto registryDecoration =
    ServiceContext::declareDecoration<LocalReshardingOperationsRegistry>();

template <typename Document>
void updateFromNamespace(OperationContext* opCtx,
                         const NamespaceString& nss,
                         LocalReshardingOperationsRegistry& registry) {
    PersistentTaskStore<Document> store(nss);
    auto role = resharding_metrics::getRoleForStateDocument<Document>();
    store.forEach(opCtx, {}, [&](const Document& doc) {
        if (resharding::excludeFromRegistry(doc)) {
            return true;
        }

        registry.registerOperation(role, doc.getCommonReshardingMetadata());
        return true;
    });
}
}  // namespace

LocalReshardingOperationsRegistry& LocalReshardingOperationsRegistry::get() {
    return registryDecoration(getGlobalServiceContext());
}

void LocalReshardingOperationsRegistry::registerOperation(
    Role role, const CommonReshardingMetadata& metadata) {
    std::unique_lock lock(_mutex);

    auto namespaceIt = _namespaceToOperations.find(metadata.getSourceNss());
    if (namespaceIt == _namespaceToOperations.end()) {
        _namespaceToOperations.emplace(
            metadata.getSourceNss(),
            UuidToOperation{{metadata.getReshardingUUID(), Operation{metadata, {role}}}});
        return;
    }
    auto& operations = namespaceIt->second;
    auto operationIt = operations.find(metadata.getReshardingUUID());
    if (operationIt == operations.end()) {
        operations.emplace(metadata.getReshardingUUID(), Operation{metadata, {role}});
        return;
    }
    auto& existingOperation = operationIt->second;
    existingOperation.roles.insert(role);
}

void LocalReshardingOperationsRegistry::unregisterOperation(
    Role role, const CommonReshardingMetadata& metadata) {
    std::unique_lock lock(_mutex);
    auto namespaceIt = _namespaceToOperations.find(metadata.getSourceNss());
    if (namespaceIt == _namespaceToOperations.end()) {
        return;
    }
    auto& operations = namespaceIt->second;
    auto operationIt = operations.find(metadata.getReshardingUUID());
    if (operationIt == operations.end()) {
        return;
    }
    auto& existingOperation = operationIt->second;
    existingOperation.roles.erase(role);
    if (existingOperation.roles.empty()) {
        operations.erase(operationIt);
        if (operations.empty()) {
            _namespaceToOperations.erase(namespaceIt);
        }
    }
}

boost::optional<LocalReshardingOperationsRegistry::Operation>
LocalReshardingOperationsRegistry::getOperation(const NamespaceString& nss) const {
    std::shared_lock lock(_mutex);
    auto namespaceIt = _namespaceToOperations.find(nss);
    if (namespaceIt == _namespaceToOperations.end()) {
        return boost::none;
    }
    auto& operations = namespaceIt->second;
    uassert(ErrorCodes::PrimarySteppedDown,
            fmt::format("Resharding operation registry transiently contains multiple operations "
                        "for namespace {}; this can occur if this node is running as a secondary",
                        nss.toStringForErrorMsg()),
            operations.size() == 1);
    return operations.begin()->second;
}

boost::optional<CommonReshardingMetadata> LocalReshardingOperationsRegistry::getDonorMetadata(
    const NamespaceString& nss) const {
    auto operation = getOperation(nss);
    if (!operation) {
        return boost::none;
    }
    if (operation->roles.contains(Role::kDonor)) {
        return operation->metadata;
    }
    return boost::none;
}

void LocalReshardingOperationsRegistry::resyncFromDisk(OperationContext* opCtx) {
    LocalReshardingOperationsRegistry resyncedRegistry;
    updateFromNamespace<ReshardingCoordinatorDocument>(
        opCtx, NamespaceString::kConfigReshardingOperationsNamespace, resyncedRegistry);
    updateFromNamespace<ReshardingDonorDocument>(
        opCtx, NamespaceString::kDonorReshardingOperationsNamespace, resyncedRegistry);
    updateFromNamespace<ReshardingRecipientDocument>(
        opCtx, NamespaceString::kRecipientReshardingOperationsNamespace, resyncedRegistry);
    std::unique_lock lock(_mutex);
    _namespaceToOperations.swap(resyncedRegistry._namespaceToOperations);
}

namespace resharding {
void throwIfReshardingInProgress(const NamespaceString& nss) {
    if (LocalReshardingOperationsRegistry::get().getOperation(nss)) {
        uasserted(ErrorCodes::ReshardCollectionInProgress,
                  "reshardCollection is in progress for namespace " + nss.toStringForErrorMsg());
    }
}
}  // namespace resharding

}  // namespace mongo
