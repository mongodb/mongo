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
#pragma once

#include "mongo/db/process_health/health_observer.h"

#include "mongo/db/service_context.h"

namespace mongo {
namespace process_health {

/**
 * Interface to conduct periodic health checks.
 * Every instance of health observer is wired internally to update the state of the FaultManager
 * when a problem is detected.
 */
class HealthObserverBase : public HealthObserver {
public:
    explicit HealthObserverBase(ServiceContext* svcCtx);
    virtual ~HealthObserverBase() = default;

    ClockSource* clockSource() const {
        return _svcCtx->getPreciseClockSource();
    }

    TickSource* tickSource() const {
        return _svcCtx->getTickSource();
    }

    ServiceContext* svcCtx() const {
        return _svcCtx;
    }

    /**
     * @return Milliseconds the shortest interval it is safe to repeat this check on.
     */
    virtual Milliseconds minimalCheckInterval() const {
        return Milliseconds(10);
    }

    // Implements the common logic for periodic checks.
    // Every observer should implement periodicCheckImpl() for specific tests.
    void periodicCheck(FaultFacetsContainerFactory& factory,
                       std::shared_ptr<executor::TaskExecutor> taskExecutor,
                       CancellationToken token) override;

    HealthObserverLivenessStats getStats() const override;

    // Common params for every health check.
    struct PeriodicHealthCheckContext {
        CancellationToken cancellationToken;
        std::shared_ptr<executor::TaskExecutor> taskExecutor;
    };

protected:
    /**
     * The main method every health observer should implement for a particular
     * health check it does.
     *
     * @return The result of a complete health check
     */
    virtual Future<HealthCheckStatus> periodicCheckImpl(
        PeriodicHealthCheckContext&& periodicCheckContext) = 0;

    // Helper method to create a status without errors.
    HealthCheckStatus makeHealthyStatus() const;

    // Make a generic error status.
    HealthCheckStatus makeSimpleFailedStatus(double severity, std::vector<Status>&& failures) const;

    HealthObserverLivenessStats getStatsLocked(WithLock) const;

    ServiceContext* const _svcCtx;

    mutable Mutex _mutex =
        MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(1), "HealthObserverBase::_mutex");

    // Indicates if there any check running to prevent running checks concurrently.
    bool _currentlyRunningHealthCheck = false;
    // Enforces the safety interval.
    Date_t _lastTimeTheCheckWasRun;
    Date_t _lastTimeCheckCompleted;
    int _completedChecksCount = 0;
    int _completedChecksWithFaultCount = 0;
};

}  // namespace process_health
}  // namespace mongo
