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
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding

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

    _registrationCount.fetchAndAdd(1);
    LOGV2(13002900,
          "Registering resharding operation in local resharding operations registry",
          "reshardingUUID"_attr = metadata.getReshardingUUID(),
          "sourceNss"_attr = metadata.getSourceNss(),
          "sourceUUID"_attr = metadata.getSourceUUID(),
          "tempReshardingNss"_attr = metadata.getTempReshardingNss(),
          "role"_attr = ReshardingMetricsCommon::getRoleName(role));

    auto namespaceIt = _namespaceToOperations.find(metadata.getSourceNss());
    if (namespaceIt == _namespaceToOperations.end()) {
        _namespaceToOperations.emplace(
            metadata.getSourceNss(),
            UuidToOperation{{metadata.getReshardingUUID(), Operation{metadata, {role}}}});
        _currentOperationCount.fetchAndAdd(1);
        return;
    }
    auto& operations = namespaceIt->second;
    auto operationIt = operations.find(metadata.getReshardingUUID());
    if (operationIt == operations.end()) {
        operations.emplace(metadata.getReshardingUUID(), Operation{metadata, {role}});
        _currentOperationCount.fetchAndAdd(1);
        return;
    }
    auto& existingOperation = operationIt->second;
    existingOperation.roles.insert(role);
}

void LocalReshardingOperationsRegistry::unregisterOperation(
    Role role, const CommonReshardingMetadata& metadata) {
    std::unique_lock lock(_mutex);

    _unregistrationCount.fetchAndAdd(1);
    LOGV2(13002901,
          "Unregistering resharding operation from local resharding operations registry",
          "reshardingUUID"_attr = metadata.getReshardingUUID(),
          "sourceNss"_attr = metadata.getSourceNss(),
          "sourceUUID"_attr = metadata.getSourceUUID(),
          "tempReshardingNss"_attr = metadata.getTempReshardingNss(),
          "role"_attr = ReshardingMetricsCommon::getRoleName(role));

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
        _currentOperationCount.fetchAndSubtract(1);
        if (operations.empty()) {
            _namespaceToOperations.erase(namespaceIt);
        }
    }
}

void LocalReshardingOperationsRegistry::clearOperationsForRole(Role role) {
    std::unique_lock lock(_mutex);

    LOGV2(13002903,
          "Clearing all resharding operations for role from local resharding operations registry",
          "role"_attr = ReshardingMetricsCommon::getRoleName(role));

    for (auto nsIt = _namespaceToOperations.begin(); nsIt != _namespaceToOperations.end();) {
        auto& operations = nsIt->second;
        for (auto opIt = operations.begin(); opIt != operations.end();) {
            opIt->second.roles.erase(role);
            if (opIt->second.roles.empty()) {
                operations.erase(opIt++);
                _currentOperationCount.fetchAndSubtract(1);
            } else {
                ++opIt;
            }
        }
        if (operations.empty()) {
            _namespaceToOperations.erase(nsIt++);
        } else {
            ++nsIt;
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

void LocalReshardingOperationsRegistry::resyncFromDisk(OperationContext* opCtx,
                                                       std::string_view reason) {
    auto countOperations = [](const auto& namespaceToOperations) {
        size_t count = 0;
        for (auto& [nss, operations] : namespaceToOperations) {
            count += operations.size();
        }
        return count;
    };

    LocalReshardingOperationsRegistry resyncedRegistry;
    updateFromNamespace<ReshardingCoordinatorDocument>(
        opCtx, NamespaceString::kConfigReshardingOperationsNamespace, resyncedRegistry);
    updateFromNamespace<ReshardingDonorDocument>(
        opCtx, NamespaceString::kDonorReshardingOperationsNamespace, resyncedRegistry);
    updateFromNamespace<ReshardingRecipientDocument>(
        opCtx, NamespaceString::kRecipientReshardingOperationsNamespace, resyncedRegistry);

    std::unique_lock lock(_mutex);
    auto previousOperationCount = countOperations(_namespaceToOperations);
    auto loadedOperationCount = countOperations(resyncedRegistry._namespaceToOperations);
    _resyncCount.fetchAndAdd(1);
    LOGV2(13002902,
          "Resyncing local resharding operations registry from disk",
          "reason"_attr = reason,
          "previousOperationCount"_attr = previousOperationCount,
          "loadedOperationCount"_attr = loadedOperationCount);
    _namespaceToOperations.swap(resyncedRegistry._namespaceToOperations);
    _currentOperationCount.store(static_cast<int64_t>(loadedOperationCount));
}

void LocalReshardingOperationsRegistry::reportForServerStatus(BSONObjBuilder* bob) const {
    BSONObjBuilder registryBuilder(bob->subobjStart("reshardingOperationsRegistry"));
    registryBuilder.append("registrations", static_cast<long long>(_registrationCount.load()));
    registryBuilder.append("unregistrations", static_cast<long long>(_unregistrationCount.load()));
    registryBuilder.append("resyncs", static_cast<long long>(_resyncCount.load()));
    registryBuilder.append("currentOperations",
                           static_cast<long long>(_currentOperationCount.load()));
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
