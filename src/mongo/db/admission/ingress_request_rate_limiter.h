// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/admission/rate_limiter.h"
#include "mongo/util/modules.h"
#include "mongo/util/periodic_runner.h"

#include <cstdint>

#include <boost/optional.hpp>

namespace mongo {
namespace admission {

class [[MONGO_MOD_PUBLIC]] IngressRequestRateLimiter {
public:
    IngressRequestRateLimiter();
    /**
     * Returns the reference to IngressRequestRateLimiter associated with the operation's service
     * context.
     */
    static IngressRequestRateLimiter& get(ServiceContext* svcCtx);

    /**
     * Attempts to admit a request into the system. Returns false if the rate limit and
     * burst capacity are exceeded AND the queue is at capacity (if configured).
     *
     * If an admission is queued, a DeferredToken is stored on a client decoration which will be
     * resolved later in the request pipeline.
     */
    bool admitRequest(Client* client);

    /**
     * Returns the canonical rejection Status that `admitRequest` returns when the rate limit is
     * exceeded. Exposed so that response-bytes builders and unit tests can reference the same
     * Status without duplicating its error code or message string.
     */
    static const Status& rejectionStatus();

    /**
     * Waits for admission to be granted. If there is no deferred token then this is a no-op,
     * otherwise it resolves the deferred token.
     */
    static Status waitForAdmission(OperationContext* opCtx);

    /**
     * Adjusts the refresh rate and burst capacity of the rate limiter.
     */
    void updateRateParameters(double refreshRatePerSec, double burstCapacitySecs);

    /**
     * Sets the maximum number of requests that may be queued waiting for a token.
     */
    void updateMaxQueueDepth(std::int64_t maxQueueDepth);

    /**
     * Called automatically when the value of the server parameter
     * ingressRequestAdmissionRatePerSec changes value.
     */
    [[MONGO_MOD_PRIVATE]] static Status onUpdateAdmissionRatePerSec(std::int32_t refreshRatePerSec);

    /**
     * Called automatically when the value of the server parameter
     * ingressRequestAdmissionBurstCapacitySecs changes value.
     */
    [[MONGO_MOD_PRIVATE]] static Status onUpdateAdmissionBurstCapacitySecs(
        double burstCapacitySecs);

    /**
     * Called automatically when the value of the server parameter
     * ingressRequestAdmissionMaxQueueDepth changes value.
     */
    [[MONGO_MOD_PRIVATE]] static Status onUpdateAdmissionMaxQueueDepth(std::int64_t maxQueueDepth);

    /**
     * Reports the ingress admission rate limiter metrics.
     */
    void appendStats(BSONObjBuilder* bob) const;

    /**
     * Starts the periodic job that samples this rate limiter's available-token gauge and pushes it
     * to the installed metrics recorder. Intended to be called once during OTel metrics
     * installation, after the ServiceContext's PeriodicRunner is available.
     */
    void installOtelMetrics(ServiceContext* svcCtx);

    /** Clears any pending deferred admission token stored on the client. */
    static void clearDeferredAdmissionToken(Client* client);
    /** Test-only helper to seed a pending DeferredToken on a client. */
    static void setDeferredAdmissionToken_forTest(Client* client, RateLimiter::DeferredToken token);
    /** Test-only helper to check if a client has a deferred admission token. */
    static bool hasDeferredAdmissionToken_forTest(Client* client);

private:
    RateLimiter _rateLimiter;

    // Owns the periodic available-token sampling job. Declared last so it is destroyed (and the job
    // stopped) before `_rateLimiter` and its recorder, which the job touches, are torn down.
    PeriodicRunner::JobAnchor _metricsSamplingJob;
};

}  // namespace admission
}  // namespace mongo
