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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kProcessHealth

#include "mongo/db/process_health/health_observer_base.h"

#include "mongo/db/process_health/deadline_future.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace process_health {

HealthObserverBase::HealthObserverBase(ServiceContext* svcCtx)
    : _svcCtx(svcCtx), _rand(PseudoRandom(SecureRandom().nextInt64())) {}

SharedSemiFuture<HealthCheckStatus> HealthObserverBase::periodicCheck(
    std::shared_ptr<executor::TaskExecutor> taskExecutor, CancellationToken token) noexcept {
    // If we have reached here, the intensity of this health observer must not be off
    {

        LOGV2_DEBUG(6007902, 2, "Start periodic health check", "observerType"_attr = getType());
        const auto now = _svcCtx->getPreciseClockSource()->now();

        auto lk = stdx::lock_guard(_mutex);
        _lastTimeTheCheckWasRun = now;
        _currentlyRunningHealthCheck = true;
    }

    _deadlineFuture = DeadlineFuture<HealthCheckStatus>::create(
        taskExecutor,
        periodicCheckImpl({token, taskExecutor})
            .onCompletion([this](StatusWith<HealthCheckStatus> status) {
                const auto now = _svcCtx->getPreciseClockSource()->now();

                auto lk = stdx::lock_guard(_mutex);
                ++_completedChecksCount;
                invariant(_currentlyRunningHealthCheck);
                _currentlyRunningHealthCheck = false;
                _lastTimeCheckCompleted = now;

                if (!status.isOK() ||
                    !HealthCheckStatus::isResolved(status.getValue().getSeverity())) {
                    ++_completedChecksWithFaultCount;
                }

                return status;
            }),
        getObserverTimeout());

    return _deadlineFuture->get();
}

HealthCheckStatus HealthObserverBase::makeHealthyStatus() const {
    return makeHealthyStatusWithType(getType());
}

HealthCheckStatus HealthObserverBase::makeHealthyStatusWithType(FaultFacetType type) {
    return HealthCheckStatus(type);
}

HealthCheckStatus HealthObserverBase::makeSimpleFailedStatus(Severity severity,
                                                             std::vector<Status>&& failures) const {
    return makeSimpleFailedStatusWithType(getType(), severity, std::move(failures));
}

HealthCheckStatus HealthObserverBase::makeSimpleFailedStatusWithType(
    FaultFacetType type, Severity severity, std::vector<Status>&& failures) {
    if (severity == Severity::kOk) {
        LOGV2_WARNING(6007903,
                      "Creating faulty health check status requires non-ok severity",
                      "observerType"_attr = type);
    }
    StringBuilder sb;
    for (const auto& s : failures) {
        sb.append(s.toString());
        sb.append(" ");
    }

    return HealthCheckStatus(type, severity, sb.stringData());
}

HealthObserverLivenessStats HealthObserverBase::getStats() const {
    auto lk = stdx::lock_guard(_mutex);
    return getStatsLocked(lk);
}

HealthObserverLivenessStats HealthObserverBase::getStatsLocked(WithLock) const {
    HealthObserverLivenessStats stats;
    stats.currentlyRunningHealthCheck = _currentlyRunningHealthCheck;
    stats.lastTimeCheckStarted = _lastTimeTheCheckWasRun;
    stats.lastTimeCheckCompleted = _lastTimeCheckCompleted;
    stats.completedChecksCount = _completedChecksCount;
    stats.completedChecksWithFaultCount = _completedChecksWithFaultCount;
    return stats;
}

Milliseconds HealthObserverBase::healthCheckJitter() const {
    return randDuration(FaultManagerConfig::kPeriodicHealthCheckMaxJitter);
}

}  // namespace process_health
}  // namespace mongo
