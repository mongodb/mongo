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


#include "mongo/db/storage/disk_space_monitor.h"

#include "mongo/db/client.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/storage/disk_space_util.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"

#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

namespace {
auto& monitorPasses = *MetricBuilder<Counter64>("diskSpaceMonitor.passes");
auto& tookAction = *MetricBuilder<Counter64>("diskSpaceMonitor.tookAction");

const auto _decoration = ServiceContext::declareDecoration<DiskSpaceMonitor>();
}  // namespace

void DiskSpaceMonitor::start(ServiceContext* svcCtx) {
    _decoration(svcCtx)._start(svcCtx);
}

void DiskSpaceMonitor::stop(ServiceContext* svcCtx) {
    _decoration(svcCtx)._stop();
}

DiskSpaceMonitor* DiskSpaceMonitor::get(ServiceContext* svcCtx) {
    return &_decoration(svcCtx);
}

void DiskSpaceMonitor::_start(ServiceContext* svcCtx) {
    LOGV2(7333401, "Starting the DiskSpaceMonitor");
    invariant(!_job, "DiskSpaceMonitor is already started");
    _dbpath = storageGlobalParams.dbpath;
    _job = svcCtx->getPeriodicRunner()->makeJob(
        PeriodicRunner::PeriodicJob{"DiskSpaceMonitor",
                                    [this](Client* client) { _run(client); },
                                    Seconds(1),
                                    true /*isKillableByStepdown*/});
    _job.start();
}

void DiskSpaceMonitor::_stop() {
    if (_job) {
        LOGV2(7333402, "Shutting down the DiskSpaceMonitor");
        _job.stop();
        _job.detach();
    }
}

int64_t DiskSpaceMonitor::registerAction(
    std::function<int64_t()> getThresholdBytes,
    std::function<void(OperationContext*, int64_t, int64_t)> act) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    invariant(_actions.try_emplace(_actionId, Action{getThresholdBytes, act}).second);
    return _actionId++;
}

void DiskSpaceMonitor::deregisterAction(int64_t actionId) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    invariant(actionId >= 0 && actionId < _actionId);
    invariant(_actions.erase(actionId));
}

void DiskSpaceMonitor::runAction(OperationContext* opCtx, int64_t id) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    auto it = _actions.find(id);
    tassert(10624900,
            fmt::format("Provided disk space monitor action id {} not found", id),
            it != _actions.end());
    _runAction(opCtx, it->second);
}

void DiskSpaceMonitor::runAllActions(OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    for (auto&& [_, action] : _actions) {
        _runAction(opCtx, action);
    }
}

void DiskSpaceMonitor::_runAction(OperationContext* opCtx, const Action& action) const {
    auto availableBytes = getAvailableDiskSpaceBytesInDbPath(_dbpath);
    auto thresholdBytes = action.getThresholdBytes();
    LOGV2_DEBUG(7333405, 2, "Available disk space", "bytes"_attr = availableBytes);
    if (availableBytes <= thresholdBytes) {
        action.act(opCtx, availableBytes, thresholdBytes);
        tookAction.increment();
    }
}

void DiskSpaceMonitor::_run(Client* client) try {
    auto opCtx = client->makeOperationContext();
    runAllActions(opCtx.get());
    monitorPasses.increment();
} catch (...) {
    LOGV2(7333404, "Caught exception in DiskSpaceMonitor", "error"_attr = exceptionToStatus());
}
}  // namespace mongo
