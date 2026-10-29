// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/admission/rate_limiter.h"
#include "mongo/db/admission/rate_limiter_counter_metrics_recorder.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/modules.h"
#include "mongo/util/periodic_runner.h"

#include <cstdint>
#include <string_view>

#include <boost/optional.hpp>

namespace mongo {

class ServiceContext;

namespace admission {

/**
 * Throttles the egress (response-send) path. A thin wrapper around `admission::RateLimiter`, stored
 * as a `ServiceContext` decoration and mirroring the `IngressRequestRateLimiter` pattern.
 *
 * See the "Egress Response Rate Limiting" section of `src/mongo/db/admission/README.md` for the
 * full design and policy rationale.
 */
class [[MONGO_MOD_PUBLIC]] EgressResponseRateLimiter {
public:
    static constexpr std::string_view kRateLimiterName = "egressResponseRateLimiter";

    EgressResponseRateLimiter();

    /**
     * Returns the EgressResponseRateLimiter associated with the given service context.
     */
    static EgressResponseRateLimiter& get(ServiceContext* svcCtx);

    /**
     * Throttles the caller until an egress-response token is available. When the queue
     * is at capacity the call bypasses the queue and returns immediately (fail-open).
     */
    Status throttle(Interruptible* interruptible, ClockSource* clockSrc);

    /**
     * Adjusts the refresh rate and burst capacity of the underlying rate limiter.
     */
    void updateRateParameters(double refreshRatePerSec, double burstCapacitySecs);

    /**
     * Sets the maximum number of egress responses that may be queued waiting for a token.
     */
    void updateMaxQueueDepth(std::int64_t maxQueueDepth);

    /**
     * Called automatically when the egressResponseRateLimiterRatePerSec server parameter changes.
     */
    [[MONGO_MOD_PRIVATE]] static Status onUpdateRatePerSec(std::int32_t refreshRatePerSec);

    /**
     * Called automatically when the egressResponseRateLimiterBurstCapacitySecs server parameter
     * changes.
     */
    [[MONGO_MOD_PRIVATE]] static Status onUpdateBurstCapacitySecs(double burstCapacitySecs);

    /**
     * Called automatically when the egressResponseRateLimiterMaxQueueDepth server parameter
     * changes.
     */
    [[MONGO_MOD_PRIVATE]] static Status onUpdateMaxQueueDepth(std::int64_t maxQueueDepth);

    /**
     * Reports the egress response rate limiter metrics.
     */
    void appendStats(BSONObjBuilder* bob) const;

    /**
     * Starts the periodic job that samples this rate limiter's available-token gauge and pushes it
     * to the installed metrics recorder. Intended to be called once during OTel metrics
     * installation, after the ServiceContext's PeriodicRunner is available.
     */
    void installOtelMetrics(ServiceContext* svcCtx);

    /**
     * Returns the underlying rate limiter's metrics recorder (for serverStatus / tests).
     */
    const RateLimiterMetricsRecorder& stats() const;
    RateLimiterMetricsRecorder& stats();

    /**
     * Returns the effective refresh rate currently configured on the underlying token bucket.
     */
    double refreshRate() const;

private:
    RateLimiter _rateLimiter;

    // Owns the periodic available-token sampling job. Declared last so it is destroyed (and the job
    // stopped) before `_rateLimiter` and its recorder, which the job touches, are torn down.
    PeriodicRunner::JobAnchor _metricsSamplingJob;
};

}  // namespace admission
}  // namespace mongo
