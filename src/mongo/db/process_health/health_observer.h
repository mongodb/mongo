// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/process_health/fault_facet.h"
#include "mongo/db/process_health/fault_manager_config.h"
#include "mongo/db/process_health/health_check_status.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <memory>

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
class [[MONGO_MOD_PUBLIC]] HealthObserver {
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
