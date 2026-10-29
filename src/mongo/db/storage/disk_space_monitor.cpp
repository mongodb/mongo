// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/storage/disk_space_monitor.h"

#include "mongo/db/client.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
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
    _dbpath = boost::filesystem::path{storageGlobalParams.dbpath};
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
    std::lock_guard<std::mutex> lock(_mutex);
    invariant(_actions.try_emplace(_actionId, Action{getThresholdBytes, act}).second);
    return _actionId++;
}

void DiskSpaceMonitor::deregisterAction(int64_t actionId) {
    std::lock_guard<std::mutex> lock(_mutex);
    invariant(actionId >= 0 && actionId < _actionId);
    invariant(_actions.erase(actionId));
}

void DiskSpaceMonitor::runAction(OperationContext* opCtx, int64_t id) {
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _actions.find(id);
    tassert(10624900,
            fmt::format("Provided disk space monitor action id {} not found", id),
            it != _actions.end());
    _runAction(opCtx, it->second);
}

void DiskSpaceMonitor::runAllActions(OperationContext* opCtx) {
    std::lock_guard<std::mutex> lock(_mutex);
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
