/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "src/mongo/db/local_catalog/ddl/replica_set_ddl_tracker.h"

#include "src/mongo/db/service_context.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

namespace {
const auto replicaSetDDLTrackerDecoration =
    ServiceContext::declareDecoration<std::unique_ptr<ReplicaSetDDLTracker>>();
}

void ReplicaSetDDLTracker::create(ServiceContext* serviceContext) {
    auto& globalDDLTracker = replicaSetDDLTrackerDecoration(serviceContext);
    globalDDLTracker = std::make_unique<ReplicaSetDDLTracker>();
}

ReplicaSetDDLTracker* ReplicaSetDDLTracker::get(ServiceContext* serviceContext) {
    return replicaSetDDLTrackerDecoration(serviceContext).get();
}

void ReplicaSetDDLTracker::registerHook(std::unique_ptr<ReplicaSetDDLHook> hook) {
    auto name = hook->getName();
    auto [_, inserted] = _ddlHooksByName.emplace(name, std::move(hook));
    invariant(inserted,
              str::stream() << "Attempted to register replica set DDL hook (" << name
                            << ") that is already registered");
    LOGV2_INFO(10898001, "Successfully registered replica set DDL hook", "hook"_attr = name);
}

ReplicaSetDDLHook* ReplicaSetDDLTracker::lookupHookByName(const StringData hookName) const {
    auto it = _ddlHooksByName.find(hookName);
    invariant(it != _ddlHooksByName.end());
    auto servicePtr = it->second.get();
    invariant(servicePtr);
    return servicePtr;
}

ReplicaSetDDLTracker::ScopedReplicaSetDDL::ScopedReplicaSetDDL(
    OperationContext* opCtx, const std::vector<NamespaceString>& namespaces)
    : _ddlTracker(ReplicaSetDDLTracker::get(opCtx->getServiceContext())),
      _opCtx(opCtx),
      _namespaces(namespaces) {
    if (_ddlTracker) {
        _ddlTracker->onBeginDDL(_opCtx, _namespaces);
    }
}

ReplicaSetDDLTracker::ScopedReplicaSetDDL::~ScopedReplicaSetDDL() {
    if (_ddlTracker) {
        _ddlTracker->onEndDDL(_opCtx, _namespaces);
    }
}

void ReplicaSetDDLTracker::onBeginDDL(OperationContext* opCtx,
                                      const std::vector<NamespaceString>& namespaces) const {
    for (auto& hook : _ddlHooksByName) {
        hook.second->onBeginDDL(opCtx, namespaces);
    }
}

void ReplicaSetDDLTracker::onEndDDL(OperationContext* opCtx,
                                    const std::vector<NamespaceString>& namespaces) const {
    for (auto& hook : _ddlHooksByName) {
        hook.second->onEndDDL(opCtx, namespaces);
    }
}

}  // namespace mongo
