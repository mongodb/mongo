// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/ddl/replica_set_ddl_tracker.h"

#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/topology/sharding_state.h"

#include <string_view>

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

ReplicaSetDDLHook* ReplicaSetDDLTracker::lookupHookByName(const std::string_view hookName) const {
    auto it = _ddlHooksByName.find(hookName);
    invariant(it != _ddlHooksByName.end());
    auto servicePtr = it->second.get();
    invariant(servicePtr);
    return servicePtr;
}

ReplicaSetDDLTracker::ScopedReplicaSetDDL::ScopedReplicaSetDDL(
    OperationContext* opCtx,
    const std::vector<NamespaceString>& namespaces,
    std::string_view ddlName,
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
                                                                std::string_view reason) {
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
