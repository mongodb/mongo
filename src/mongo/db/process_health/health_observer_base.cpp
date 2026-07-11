// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/process_health/health_observer_base.h"

#include "mongo/base/status_with.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/db/process_health/deadline_future.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/future_impl.h"

#include <mutex>
#include <type_traits>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kProcessHealth


namespace mongo {
namespace process_health {

HealthObserverBase::HealthObserverBase(ServiceContext* svcCtx)
    : _svcCtx(svcCtx), _rand(PseudoRandom(SecureRandom().nextInt64())) {}

SharedSemiFuture<HealthCheckStatus> HealthObserverBase::periodicCheck(
    std::shared_ptr<executor::TaskExecutor> taskExecutor, CancellationToken token) {
    // If we have reached here, the intensity of this health observer must not be off
    {

        LOGV2_DEBUG(6007902, 2, "Start periodic health check", "observerType"_attr = getType());
        const auto now = _svcCtx->getPreciseClockSource()->now();

        auto lk = std::lock_guard(_mutex);
        _lastTimeTheCheckWasRun = now;
        _currentlyRunningHealthCheck = true;
    }

    Future<HealthCheckStatus> healthCheckResult;

    try {
        healthCheckResult = periodicCheckImpl({token, taskExecutor});
    } catch (const DBException& e) {
        LOGV2_DEBUG(6728001,
                    2,
                    "Health observer failed due to an exception",
                    "observerType"_attr = getType(),
                    "errorCode"_attr = e.code(),
                    "reason"_attr = e.reason());

        healthCheckResult = makeSimpleFailedStatus(Severity::kFailure, {e.toStatus()});
    }

    _deadlineFuture = DeadlineFuture<HealthCheckStatus>::create(
        taskExecutor,
        std::move(healthCheckResult).onCompletion([this](StatusWith<HealthCheckStatus> status) {
            const auto now = _svcCtx->getPreciseClockSource()->now();

            auto lk = std::lock_guard(_mutex);
            ++_completedChecksCount;
            invariant(_currentlyRunningHealthCheck);
            _currentlyRunningHealthCheck = false;
            _lastTimeCheckCompleted = now;

            if (!status.isOK() || !HealthCheckStatus::isResolved(status.getValue().getSeverity())) {
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
    auto lk = std::lock_guard(_mutex);
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
