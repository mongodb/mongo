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

namespace mongo {

namespace {
const auto registryDecoration =
    ServiceContext::declareDecoration<LocalReshardingOperationsRegistry>();
}

LocalReshardingOperationsRegistry& LocalReshardingOperationsRegistry::get() {
    return registryDecoration(getGlobalServiceContext());
}

void LocalReshardingOperationsRegistry::registerOperation(
    Role role, const CommonReshardingMetadata& metadata) {
    std::unique_lock lock(_mutex);
    auto it = _operations.find(metadata.getSourceNss());
    if (it == _operations.end()) {
        _operations.emplace(metadata.getSourceNss(), Operation{metadata, {role}});
        return;
    }
    auto& existingOperation = it->second;
    uassert(
        ErrorCodes::ConflictingOperationInProgress,
        fmt::format("Unable to register new resharding operation for namespace {} because another "
                    "operation with different parameters is already in progress for that namespace",
                    metadata.getSourceNss().toStringForErrorMsg()),
        metadata == existingOperation.metadata);
    existingOperation.roles.insert(role);
}

void LocalReshardingOperationsRegistry::unregisterOperation(
    Role role, const CommonReshardingMetadata& metadata) {
    std::unique_lock lock(_mutex);
    auto it = _operations.find(metadata.getSourceNss());
    if (it == _operations.end()) {
        return;
    }
    auto& existingOperation = it->second;
    if (metadata != existingOperation.metadata) {
        return;
    }
    existingOperation.roles.erase(role);
    if (existingOperation.roles.empty()) {
        _operations.erase(it);
    }
}

boost::optional<LocalReshardingOperationsRegistry::Operation>
LocalReshardingOperationsRegistry::getOperation(const NamespaceString& nss) const {
    std::shared_lock lock(_mutex);
    auto it = _operations.find(nss);
    if (it == _operations.end()) {
        return boost::none;
    }
    return it->second;
}

}  // namespace mongo
