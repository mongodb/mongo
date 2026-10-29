// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/db/topology/periodic_replica_set_configshard_maintenance_mode_checker.h"

#include "mongo/db/client.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

namespace {
static constexpr Milliseconds kCheckIntervalMS{Minutes{15}};

const auto serviceDecoration =
    ServiceContext::declareDecoration<PeriodicReplicaSetConfigShardMaintenanceModeChecker>();
}  // namespace

auto PeriodicReplicaSetConfigShardMaintenanceModeChecker::get(ServiceContext* serviceContext)
    -> PeriodicReplicaSetConfigShardMaintenanceModeChecker& {
    auto& jobContainer = serviceDecoration(serviceContext);
    jobContainer._init(serviceContext);

    return jobContainer;
}

auto PeriodicReplicaSetConfigShardMaintenanceModeChecker::operator*() const -> PeriodicJobAnchor& {
    std::lock_guard lk(_mutex);
    return *_anchor;
}

auto PeriodicReplicaSetConfigShardMaintenanceModeChecker::operator->() const -> PeriodicJobAnchor* {
    std::lock_guard lk(_mutex);
    return _anchor.get();
}

PeriodicReplicaSetConfigShardMaintenanceModeChecker::operator bool() const {
    return _anchor.get();
}

void PeriodicReplicaSetConfigShardMaintenanceModeChecker::_init(ServiceContext* serviceContext) {
    std::lock_guard lk(_mutex);
    if (_anchor) {
        return;
    }

    auto periodicRunner = serviceContext->getPeriodicRunner();
    if (!periodicRunner) {
        // This can happen if we exit very early during startup.
        return;
    }

    PeriodicRunner::PeriodicJob job(
        "PeriodicReplicaSetConfigShardMaintenanceModeChecker",
        [](Client*) {
            LOGV2_WARNING(10718700,
                          "The server was started with --replicaSetConfigShardMaintenanceMode. "
                          "This mode is intended only for replica set / config shard transitions "
                          "or config server maintenance. If you are not actively performing one of "
                          "these operations (e.g., converting to/from a sharded cluster with an "
                          "embedded config server), please exit this mode as soon as possible.");
        },
        kCheckIntervalMS,
        false);

    _anchor = std::make_shared<PeriodicJobAnchor>(periodicRunner->makeJob(std::move(job)));
}

}  // namespace mongo
