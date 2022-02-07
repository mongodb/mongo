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

#include "mongo/db/process_health/fault_facet.h"
#include "mongo/db/process_health/fault_manager_config.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/future.h"

namespace mongo {
namespace process_health {

/**
 * Liveness data and stats.
 */
struct HealthObserverLivenessStats {
    // true is this observer is currently running a health check.
    bool currentlyRunningHealthCheck = false;
    // When the last or current check started, depending if currently
    // running one.
    Date_t lastTimeCheckStarted = Date_t::max();
    // When the last check completed (not the current one).
    Date_t lastTimeCheckCompleted = Date_t::max();
    // Incremented when a check is done.
    int completedChecksCount = 0;
    // Incremented when check completed with fault.
    // This doesn't take into account critical vs non-critical.
    int completedChecksWithFaultCount = 0;

    BSONObj toBSON() const {
        BSONObjBuilder builder;
        builder.append("currentlyRunningHealthCheck", currentlyRunningHealthCheck);
        builder.append("lastTimeCheckStarted", lastTimeCheckStarted);
        builder.append("lastTimeCheckCompleted", lastTimeCheckCompleted);
        builder.append("completedChecksCount", completedChecksCount);
        builder.append("completedChecksWithFaultCount", completedChecksWithFaultCount);
        return builder.obj();
    }
};

/**
 * Interface to conduct periodic health checks.
 */
class HealthObserver {
public:
    virtual ~HealthObserver() = default;

    /**
     * Health observer of this type is unique and can only create the fault facet
     * of the same type.
     *
     * @return FaultFacetType of this health observer.
     */
    virtual FaultFacetType getType() const = 0;

    /**
     * Triggers health check. The implementation should not block to wait for the completion
     * of this check.
     *
     * @param factory Interface to get or create the factory of faults.
     */
    virtual SharedSemiFuture<HealthCheckStatus> periodicCheck(
        std::shared_ptr<executor::TaskExecutor> taskExecutor, CancellationToken token) = 0;

    virtual HealthObserverLivenessStats getStats() const = 0;

    /**
     * Value used to introduce jitter between health check invocations.
     */
    virtual Milliseconds healthCheckJitter() const = 0;

    /**
     * Timeout value enforced on an individual health check.
     */
    virtual Milliseconds getObserverTimeout() const = 0;

    /**
     * Returns false if the health observer is missing some configuration it needs for its health
     * checks. In the case of faulty configuration, make sure to log any helpful messages within
     * this method.
     */
    virtual bool isConfigured() const = 0;
};

}  // namespace process_health
}  // namespace mongo
