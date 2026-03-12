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

#include "mongo/db/shard_role/ddl/replica_set_ddl_tracker.h"

#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/topology/sharding_state.h"

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
    OperationContext* opCtx,
    const std::vector<NamespaceString>& namespaces,
    StringData ddlName,
    const ReplicaSetDDLOptions& options)
    : _ddlTracker(ReplicaSetDDLTracker::get(opCtx->getServiceContext())),
      _opCtx(opCtx),
      _namespaces(namespaces) {
    // During promotion to sharded, this check can race with the one for direct shard commands in
    // DirectConnectionDDLHook. Prefer checking _before_ the DirectConnectionDDLHook, which may lead
    // to a direct shard command taking the DDL lock (unnecessary but correct), rather than _after_
    // the DirectConnectionDDLHook, which may lead to a replica set DDL not taking the DDL lock.
    bool shardingEnabled = ShardingState::get(opCtx)->enabled();

    if (_ddlTracker) {
        _ddlTracker->onBeginDDL(_opCtx, _namespaces);
    }

    if (options.acquireDDLLocks && !shardingEnabled) {
        acquireDDLLocks(opCtx, ddlName);
    }
}

ReplicaSetDDLTracker::ScopedReplicaSetDDL::~ScopedReplicaSetDDL() {
    if (_ddlTracker) {
        _ddlTracker->onEndDDL(_opCtx, _namespaces);
    }
}

void ReplicaSetDDLTracker::ScopedReplicaSetDDL::acquireDDLLocks(OperationContext* opCtx,
                                                                StringData reason) {
    // (Ignore FCV check): DDL locks are currently only acquired to serialize timeseries DDLs,
    // so don't acquire them if viewless timeseries is not enabled to minimize risk before release.
    if (!gFeatureFlagCreateViewlessTimeseriesCollections.isEnabledAndIgnoreFCVUnsafe()) {
        return;
    }

    opCtx->setAlwaysInterruptAtStepDownOrUp_UNSAFE();

    // Sort the locks (via std::set's ordering) to avoid deadlock
    std::set<NamespaceString> _sortedNamespaces;
    for (const auto& nss : _namespaces) {
        _sortedNamespaces.insert(
            nss.isTimeseriesBucketsCollection() ? nss.getTimeseriesViewNamespace() : nss);
    }

    auto locker = shard_role_details::getLocker(opCtx);
    constexpr bool waitForRecovery = false;
    for (const auto& nss : _sortedNamespaces) {
        if (nss.isDbOnly()) {
            _ddlLocks.emplace_back(
                opCtx, locker, nss.dbName(), reason, LockMode::MODE_X, waitForRecovery);
        } else {
            _ddlLocks.emplace_back(
                opCtx, locker, nss.dbName(), reason, LockMode::MODE_IX, waitForRecovery);
            _ddlLocks.emplace_back(opCtx, locker, nss, reason, LockMode::MODE_X, waitForRecovery);
        }
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
