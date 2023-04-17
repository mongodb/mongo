/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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


#include "mongo/db/process_health/progress_monitor.h"

#include "mongo/db/process_health/fault_manager.h"
#include "mongo/db/process_health/health_monitoring_server_parameters_gen.h"
#include "mongo/db/process_health/health_observer.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kProcessHealth


namespace mongo {
namespace process_health {

ProgressMonitor::ProgressMonitor(FaultManager* faultManager,
                                 ServiceContext* svcCtx,
                                 std::function<void(std::string cause)> crashCb)
    : _faultManager(faultManager), _svcCtx(svcCtx), _crashCb(crashCb) {
    // Start the monitor thread, this should happen after all observers are initialized.
    _progressMonitorThread = stdx::thread([this] { _progressMonitorLoop(); });
}

void ProgressMonitor::join() {
    _terminate.store(true);

    // The _progressMonitorThread is watching the _taskExecutor join()
    // completion and thus can be joined only after the _taskExecutor completes.
    LOGV2(5936602, "Stopping the periodic health checks liveness monitor");
    if (_progressMonitorThread.joinable()) {
        _progressMonitorThread.join();
    }
}

void ProgressMonitor::progressMonitorCheck(std::function<void(std::string cause)> crashCb) {
    std::vector<HealthObserver*> observers = _faultManager->getHealthObservers();
    const auto now = _svcCtx->getPreciseClockSource()->now();
    std::vector<HealthObserver*> secondPass;

    // Check the liveness of every health observer.
    for (auto observer : observers) {
        const auto stats = observer->getStats();
        const auto observerType = observer->getType();
        const auto observerIntensity =
            _faultManager->getConfig().getHealthObserverIntensity(observerType);

        if (observerIntensity != HealthObserverIntensityEnum::kCritical) {
            LOGV2_DEBUG(6290401,
                        2,
                        "Skip checking progress of not critical health observer",
                        "observerType"_attr = (str::stream() << observerType),
                        "intensity"_attr = HealthObserverIntensity_serializer(observerIntensity));
            continue;
        }

        // Special case: if the health observer is enabled but did not run
        // for a very long time, it could be a race. We should check it later.
        if (!stats.currentlyRunningHealthCheck &&
            now - stats.lastTimeCheckStarted >
                _faultManager->getConfig().getPeriodicLivenessDeadline() * 2) {
            secondPass.push_back(observer);
            continue;
        }

        if (stats.currentlyRunningHealthCheck &&
            now - stats.lastTimeCheckStarted >
                _faultManager->getConfig().getPeriodicLivenessDeadline()) {

            // Crash because this health checker is running for too long.
            crashCb(str::stream() << "Health observer " << observer->getType()
                                  << " is still running since "
                                  << stats.lastTimeCheckStarted.toString());
        }
    }

    if (secondPass.empty()) {
        return;
    }

    auto longestIntervalHealthObserver = *std::max_element(
        secondPass.begin(), secondPass.end(), [&](const auto& lhs, const auto& rhs) {
            auto lhs_interval =
                _faultManager->getConfig().getPeriodicHealthCheckInterval(lhs->getType());
            auto rhs_interval =
                _faultManager->getConfig().getPeriodicHealthCheckInterval(rhs->getType());
            return lhs_interval < rhs_interval;
        });

    auto longestInterval = _faultManager->getConfig().getPeriodicHealthCheckInterval(
        longestIntervalHealthObserver->getType());

    sleepFor(longestInterval * 2);
    // The observer is enabled but did not run for a while. Sleep two cycles
    // and check again. Note: this should be rare.
    for (auto observer : secondPass) {
        const auto stats = observer->getStats();

        if (!_faultManager->getConfig().isHealthObserverEnabled(observer->getType()) &&
            !stats.currentlyRunningHealthCheck &&
            now - stats.lastTimeCheckStarted >
                _faultManager->getConfig().getPeriodicLivenessDeadline() * 2) {

            // Crash because this health checker was never started.
            crashCb(str::stream() << "Health observer " << observer->getType()
                                  << " did not run since "
                                  << stats.lastTimeCheckStarted.toString());
            continue;
        }
    }
}

void ProgressMonitor::_progressMonitorLoop() {
    Client::initThread("FaultManagerProgressMonitor"_sd, _svcCtx, nullptr);
    static const int kSleepsPerInterval = 10;

    // TODO(SERVER-74659): Please revisit if this thread could be made killable.
    {
        stdx::lock_guard<Client> lk(cc());
        cc().setSystemOperationUnkillableByStepdown(lk);
    }

    while (!_terminate.load()) {
        progressMonitorCheck(_crashCb);

        const auto interval = duration_cast<Microseconds>(
            _faultManager->getConfig().getPeriodicLivenessCheckInterval());

        // Breaking up the sleeping interval to check for `_terminate` more often.
        for (int i = 0; i < kSleepsPerInterval && !_terminate.load(); ++i) {
            sleepFor(interval / kSleepsPerInterval);
        }
    }
}

}  // namespace process_health
}  // namespace mongo
