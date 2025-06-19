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

#include "mongo/base/counter.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/admission/rate_limiter.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/versioned_value.h"

#include <cstdint>

namespace mongo::transport {
/**
 * The SessionEstablishmentRateLimiter encapsulates the logic and metrics for rate-limiting new
 * session establishment in order to avoid overloading the server.
 *
 * The behavior of SessionEstablishmentRateLimiter is influenced by the following server
 * parameters:
 *
 * - ingressConnectionEstablishmentRatePerSec
 * - ingressConnectionEstablishmentBurstCapacitySecs
 * - ingressConnectionEstablishmentMaxQueueDepth
 *
 * SessionEstablishmentRateLimiter is used in SessionWorkflow if the following feature flag
 * server parameter is true:
 *
 * - featureFlagRateLimitIngressConnectionEstablishment
 */
class SessionEstablishmentRateLimiter {
public:
    SessionEstablishmentRateLimiter();

    /**
     * Returns the rate limiter for ingress connections associated with the specified service
     * context for the specified protocol. Returns nullptr if the service context does not have
     * a matching ingress transport layer.
     */
    static SessionEstablishmentRateLimiter* get(ServiceContext&, TransportProtocol);

    /**
     * New Sessions should call into this function to respect the configured establishment rate
     * limit. If the session's IP is not exempt from rate-limiting, it will block until it is let
     * through by the underlying rate limiter.
     */
    Status throttleIfNeeded(Client* client);

    // Stats

    int64_t queued() const {
        return _rateLimiter.queued();
    }

    int64_t rejected() const {
        return _rateLimiter.stats().rejectedAdmissions.get();
    }

    /** These stats go in the "connections" section of the server status. **/
    void appendStatsConnections(BSONObjBuilder* bob) const;

    /** These stats go in the "queues" section of the server status. **/
    void appendStatsQueues(BSONObjBuilder* bob) const {
        _rateLimiter.appendStats(bob);
    }

    // Configuration Options

    void updateRateParameters(double refreshRatePerSec, double burstSize) {
        _rateLimiter.updateRateParameters(refreshRatePerSec, burstSize);
    }

    void setMaxQueueDepth(int64_t maxQueueDepth) {
        _rateLimiter.setMaxQueueDepth(maxQueueDepth);
    }

private:
    admission::RateLimiter _rateLimiter;
    VersionedValue<CIDRList>::Snapshot _maxEstablishingConnsOverride;

    // Stats
    Counter64 _exempted;
    Counter64 _interruptedDueToClientDisconnect;
};
}  // namespace mongo::transport
