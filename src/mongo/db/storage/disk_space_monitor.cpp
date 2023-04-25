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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

#include "mongo/db/storage/disk_space_monitor.h"

#include "mongo/db/client.h"
#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/storage/disk_space_util.h"
#include "mongo/db/storage/storage_parameters_gen.h"
#include "mongo/logv2/log.h"

namespace mongo {

CounterMetric monitorPasses("diskSpaceMonitor.passes");
CounterMetric tookAction("diskSpaceMonitor.tookAction");

namespace {
static const auto _decoration = ServiceContext::declareDecoration<DiskSpaceMonitor>();
}  // namespace

void DiskSpaceMonitor::start(ServiceContext* svcCtx) {
    auto storageEngine = svcCtx->getStorageEngine();
    const bool filesNotAllInSameDirectory =
        storageEngine->isUsingDirectoryPerDb() || storageEngine->isUsingDirectoryForIndexes();
    if (filesNotAllInSameDirectory) {
        LOGV2(7333400,
              "The DiskSpaceMonitor will not run when the storage engine stores data files in "
              "different directories");
        return;
    }

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
    _job = svcCtx->getPeriodicRunner()->makeJob(PeriodicRunner::PeriodicJob{
        "DiskSpaceMonitor",
        [this](Client* client) { _run(client); },
        Seconds(1),
        // TODO(SERVER-74657): Please revisit if this periodic job could be made killable.
        false /*isKillableByStepdown*/});
    _job.start();
}

void DiskSpaceMonitor::_stop() {
    if (_job) {
        LOGV2(7333402, "Shutting down the DiskSpaceMonitor");
        _job.stop();
        _job.detach();
    }
}

void DiskSpaceMonitor::registerAction(std::unique_ptr<Action> action) {
    stdx::lock_guard<Latch> lock(_mutex);
    _actions.push_back(std::move(action));
}

void DiskSpaceMonitor::takeAction(OperationContext* opCtx, int64_t availableBytes) {
    stdx::lock_guard<Latch> lock(_mutex);

    for (auto& action : _actions) {
        if (availableBytes <= action->getThresholdBytes()) {
            action->act(opCtx, availableBytes);
            tookAction.increment();
        }
    }
}

void DiskSpaceMonitor::_run(Client* client) try {
    auto opCtx = client->makeOperationContext();

    const auto availableBytes = getAvailableDiskSpaceBytesInDbPath();
    LOGV2_DEBUG(7333405, 2, "Available disk space", "bytes"_attr = availableBytes);
    takeAction(opCtx.get(), availableBytes);
    monitorPasses.increment();
} catch (...) {
    LOGV2(7333404, "Caught exception in DiskSpaceMonitor", "error"_attr = exceptionToStatus());
}
}  // namespace mongo
