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

#include "mongo/base/status.h"
#include "mongo/db/process_health/deadline_future.h"
#include "mongo/db/process_health/fault_manager_config.h"
#include "mongo/db/process_health/health_check_status.h"
#include "mongo/db/process_health/health_observer.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/task_executor.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/hierarchical_acquisition.h"
#include "mongo/util/tick_source.h"
#include "mongo/util/time_support.h"

#include <memory>
#include <vector>

#include <boost/move/utility_core.hpp>

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
    ~HealthObserverBase() override = default;

    ClockSource* clockSource() const {
        return _svcCtx->getPreciseClockSource();
    }

    TickSource* tickSource() const {
        return _svcCtx->getTickSource();
    }

    ServiceContext* svcCtx() const {
        return _svcCtx;
    }


    // Implements the common logic for periodic checks.
    // Every observer should implement periodicCheckImpl() for specific tests.
    SharedSemiFuture<HealthCheckStatus> periodicCheck(
        std::shared_ptr<executor::TaskExecutor> taskExecutor, CancellationToken token) override;

    HealthCheckStatus makeHealthyStatus() const;
    static HealthCheckStatus makeHealthyStatusWithType(FaultFacetType type);

    HealthCheckStatus makeSimpleFailedStatus(Severity severity,
                                             std::vector<Status>&& failures) const;
    static HealthCheckStatus makeSimpleFailedStatusWithType(FaultFacetType type,
                                                            Severity severity,
                                                            std::vector<Status>&& failures);

    HealthObserverLivenessStats getStats() const override;
    Milliseconds healthCheckJitter() const override;

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

    HealthObserverLivenessStats getStatsLocked(WithLock) const;

    template <typename T>
    T randDuration(T upperBound) const {
        auto upperCount = durationCount<T>(upperBound);
        stdx::lock_guard lock(_mutex);
        auto resultCount = _rand.nextInt64(upperCount);
        return T(resultCount);
    }

    ServiceContext* const _svcCtx;

    mutable stdx::mutex _mutex;

    // Indicates if there any check running to prevent running checks concurrently.
    bool _currentlyRunningHealthCheck = false;
    std::shared_ptr<const DeadlineFuture<HealthCheckStatus>> _deadlineFuture;
    // Enforces the safety interval.
    Date_t _lastTimeTheCheckWasRun;
    Date_t _lastTimeCheckCompleted;
    int _completedChecksCount = 0;
    int _completedChecksWithFaultCount = 0;

    mutable PseudoRandom _rand;
};

}  // namespace process_health
}  // namespace mongo
