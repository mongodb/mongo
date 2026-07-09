/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/db/admission/rate_limiter.h"
#include "mongo/util/modules.h"

#include <cstdint>

#include <boost/optional.hpp>

namespace mongo {
namespace admission {

class MONGO_MOD_PUBLIC IngressRequestRateLimiter {
public:
    IngressRequestRateLimiter();
    /**
     * Returns the reference to IngressRequestRateLimiter associated with the operation's service
     * context.
     */
    static IngressRequestRateLimiter& get(ServiceContext* svcCtx);

    /**
     * Attempts to admit a request into the system. Returns an error status if the rate limit and
     * burst capacity are exceeded AND the queue is at capacity (if configured).
     *
     * If an admission is queued a DeferredToken is stored on a client decoration which will be
     * resolved later in the request pipeline.
     */
    Status admitRequest(Client* client);

    /**
     * Waits for admission to be granted. If there is no deferred token then this is a no-op,
     * otherwise it resolves the deferred token.
     *
     * If isExemptFromAdmissionControl is true, the deferred token is marked exempt instead of
     * waited on, returning the borrowed token to the bucket and releasing its queue slot.
     *
     * Exemption is determined here (rather than in admitRequest) because it
     * depends on command-level state that is not available at the point of message read. As a
     * consequence, the exemption can only take effect for requests that successfully reserved a
     * queue slot in admitRequest. A rate-exceeded request that was rejected up front because
     * queueing was disabled or the queue was full will not reach this function. See README.md for
     * the rationale.
     */
    static Status waitForAdmission(OperationContext* opCtx, bool isExemptFromAdmissionControl);

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
    MONGO_MOD_PRIVATE static Status onUpdateAdmissionRatePerSec(std::int32_t refreshRatePerSec);

    /**
     * Called automatically when the value of the server parameter
     * ingressRequestAdmissionBurstCapacitySecs changes value.
     */
    MONGO_MOD_PRIVATE static Status onUpdateAdmissionBurstCapacitySecs(double burstCapacitySecs);

    /**
     * Called automatically when the value of the server parameter
     * ingressRequestAdmissionMaxQueueDepth changes value.
     */
    MONGO_MOD_PRIVATE static Status onUpdateAdmissionMaxQueueDepth(std::int64_t maxQueueDepth);

    /**
     * Reports the ingress admission rate limiter metrics.
     */
    void appendStats(BSONObjBuilder* bob) const;

    /**
     * Returns true if the client's application or driver name matches the
     * ingressRequestRateLimiterApplicationExemptions list. This is only for testing, but the
     * module linter does not like a _forTest function being called from outside of the module.
     *
     * TODO(SERVER-114130): Remove this function once failpoint routes through regular rate limiter
     * pathway.
     */
    static bool isAppNameExempted(Client* client);

    /** Clears any pending deferred admission token stored on the client. */
    static void clearDeferredAdmissionToken(Client* client);
    /** Test-only helper to seed a pending DeferredToken on a client. */
    static void setDeferredAdmissionToken_forTest(Client* client, RateLimiter::DeferredToken token);
    /** Test-only helper to check if a client has a deferred admission token. */
    static bool hasDeferredAdmissionToken_forTest(Client* client);

private:
    RateLimiter _rateLimiter;
};

}  // namespace admission
}  // namespace mongo
