// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/counter.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/admission/rate_limiter.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/modules.h"
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
 * - ingressConnectionEstablishmentRateLimiterBypass
 *
 * SessionEstablishmentRateLimiter is used in SessionWorkflow if the following feature flag
 * server parameter is true:
 *
 * - featureFlagRateLimitIngressConnectionEstablishment
 */
class [[MONGO_MOD_PUBLIC]] SessionEstablishmentRateLimiter {
public:
    SessionEstablishmentRateLimiter();

    virtual ~SessionEstablishmentRateLimiter() = default;

    /**
     * Returns the rate limiter for ingress connections associated with the specified service
     * context for the specified protocol. Returns nullptr if the service context does not have
     * a matching ingress transport layer.
     */
    static SessionEstablishmentRateLimiter* get(ServiceContext&, TransportProtocol);

    /**
     * New Sessions should call into this function to respect the configured establishment rate
     * limit. Sessions connected to the priority port or whose IP is in the exemption list
     * will be exempt. All other sessions will block until admitted by the rate limiter.
     */
    Status throttleIfNeeded(Client* client);

    // Stats

    virtual int64_t queued() const {
        return _rateLimiter.queued();
    }

    virtual int64_t rejected() const {
        return _rateLimiter.stats().rejectedAdmissions();
    }

    virtual int64_t exempted() const {
        return _rateLimiter.stats().exemptedAdmissions();
    }

    virtual int64_t interruptedDueToClientDisconnect() const {
        return _interruptedDueToClientDisconnect.get();
    }

    /** These stats go in the "connections" section of the server status. **/
    void appendStatsConnections(BSONObjBuilder* bob) const;

    /** These stats go in the "queues" section of the server status. **/
    void appendStatsQueues(BSONObjBuilder* bob) const {
        _rateLimiter.appendStats(bob);
    }

    // Configuration Options

    void updateRateParameters(double refreshRatePerSec, double burstCapacitySecs) {
        _rateLimiter.updateRateParameters(refreshRatePerSec, burstCapacitySecs);
    }

    void setMaxQueueDepth(int64_t maxQueueDepth) {
        _rateLimiter.setMaxQueueDepth(maxQueueDepth);
    }

private:
    admission::RateLimiter _rateLimiter;

    // Stats
    Counter64 _interruptedDueToClientDisconnect;
};
}  // namespace mongo::transport
