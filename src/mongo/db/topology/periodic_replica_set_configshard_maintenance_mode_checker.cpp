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
    stdx::lock_guard lk(_mutex);
    return *_anchor;
}

auto PeriodicReplicaSetConfigShardMaintenanceModeChecker::operator->() const -> PeriodicJobAnchor* {
    stdx::lock_guard lk(_mutex);
    return _anchor.get();
}


void PeriodicReplicaSetConfigShardMaintenanceModeChecker::_init(ServiceContext* serviceContext) {
    stdx::lock_guard lk(_mutex);
    if (_anchor) {
        return;
    }

    auto periodicRunner = serviceContext->getPeriodicRunner();
    invariant(periodicRunner);

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
