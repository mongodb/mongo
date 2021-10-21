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

#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"

namespace mongo {
namespace process_health {

HealthObserverBase::HealthObserverBase(ClockSource* clockSource, TickSource* tickSource)
    : _clockSource(clockSource), _tickSource(tickSource) {}

void HealthObserverBase::periodicCheck(FaultFacetsContainerFactory& factory,
                                       std::shared_ptr<executor::TaskExecutor> taskExecutor) {
    // TODO(SERVER-59368): fix this for runtime options support.
    if (getIntensity() == HealthObserverIntensity::kOff) {
        return;
    }

    {
        auto lk = stdx::lock_guard(_mutex);
        if (_currentlyRunningHealthCheck) {
            return;
        }

        if (_clockSource->now() - _lastTimeTheCheckWasRun < minimalCheckInterval()) {
            return;
        }
        _lastTimeTheCheckWasRun = _clockSource->now();

        LOGV2_DEBUG(6007902, 2, "Start periodic health check", "observerType"_attr = getType());

        _currentlyRunningHealthCheck = true;
    }

    // Do the health check.
    taskExecutor->schedule([this, &factory](Status status) {
        periodicCheckImpl({})
            .then([this, &factory](HealthCheckStatus&& checkStatus) mutable {
                factory.updateWithCheckStatus(std::move(checkStatus));
            })
            .onCompletion([this](Status status) {
                if (!status.isOK()) {
                    // Health checkers should not throw, they should return FaultFacetPtr.
                    LOGV2_ERROR(
                        6007901, "Unexpected failure during health check", "status"_attr = status);
                }
                auto lk = stdx::lock_guard(_mutex);
                invariant(_currentlyRunningHealthCheck);
                _currentlyRunningHealthCheck = false;
            })
            .getAsync([this](Status status) {});
    });
}

HealthObserverIntensity HealthObserverBase::getIntensity() {
    return _intensity;
}

HealthCheckStatus HealthObserverBase::makeHealthyStatus() const {
    return HealthCheckStatus(getType());
}

HealthCheckStatus HealthObserverBase::makeSimpleFailedStatus(double severity,
                                                             std::vector<Status>&& failures) const {
    if (severity <= 0) {
        LOGV2_WARNING(6007903,
                      "Creating faulty health check status requires positive severity",
                      "observerType"_attr = getType());
    }
    StringBuilder sb;
    for (const auto& s : failures) {
        sb.append(s.toString());
        sb.append(" ");
    }

    return HealthCheckStatus(getType(), severity, sb.stringData());
}

}  // namespace process_health
}  // namespace mongo
