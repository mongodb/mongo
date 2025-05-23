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
#include "mongo/transport/session.h"
#include "mongo/transport/transport_options_gen.h"
#include "mongo/util/versioned_value.h"

namespace mongo::transport {
/**
 * The SessionEstablishmentRateLimiter encapsulates the logic and metrics for rate-limiting new
 * session establishment in order to avoid overloading the server.
 */
class SessionEstablishmentRateLimiter {
public:
    SessionEstablishmentRateLimiter()
        : _rateLimiter(gIngressConnectionEstablishmentRatePerSec.load(),
                       gIngressConnectionEstablishmentBurstSize.load(),
                       gIngressConnectionEstablishmentMaxQueueDepth.load()) {}

    /**
     * New Sessions should call into this function to respect the configured establishment rate
     * limit. If the session's IP is not exempt from rate-limiting, it will block until it is let
     * through by the underlying rate limiter.
     */
    Status throttleIfNeeded(Client* client) {
        // We can short-circuit if the rate limit is unlimited (aka the feature is off).
        if (gIngressConnectionEstablishmentRatePerSec.loadRelaxed() ==
            std::numeric_limits<int>::max()) {
            return Status::OK();
        }

        // Check if the session is exempt from rate limiting based on its IP.
        serverGlobalParams.maxEstablishingConnsOverride.refreshSnapshot(
            _maxEstablishingConnsOverride);
        if (_maxEstablishingConnsOverride &&
            client->session()->isExemptedByCIDRList(*_maxEstablishingConnsOverride)) {
            _exempted.incrementRelaxed();
            return Status::OK();
        }

        _added.incrementRelaxed();  // TODO SERVER-104413: Move this logic inside the rate limiter.
        ON_BLOCK_EXIT([&] { _removed.incrementRelaxed(); });

        // Create an opCtx for interruptibility and to make queued waiters return tokens
        // when the client has disconnected.
        auto establishmentOpCtx = client->makeOperationContext();
        establishmentOpCtx->markKillOnClientDisconnect();

        // Acquire a token or block until one becomes available.
        Status s = _rateLimiter.acquireToken(establishmentOpCtx.get());
        if (MONGO_unlikely(!s.isOK())) {
            if (s.code() == ErrorCodes::ClientDisconnect) {
                _interuptedDueToClientDisconnect.incrementRelaxed();
            } else if (s.code() == ErrorCodes::TemporarilyUnavailable) {
                // TODO SERVER-104413: Move this logic inside the rate limiter.
                _rejected.incrementRelaxed();
            }
        }

        return s;
    }

    // Stats

    size_t queued() const {
        return _added.get() - _removed.get();
    }

    size_t rejected() const {
        return _rejected.get();
    }

    void appendStats(BSONObjBuilder* bob) const {
        // TODO SERVER-104413: Replace rejected and queued metrics with calculations from underlying
        // RateLimiter.
        bob->append("queued", _added.get() - _removed.get());

        BSONObjBuilder subBuilder = bob->subobjStart("establishmentRateLimit");
        subBuilder.append("totalRejected", _rejected.get());
        subBuilder.append("totalExempted", _exempted.get());
        subBuilder.append("totalInterruptedDueToClientDisconnect",
                          _interuptedDueToClientDisconnect.get());
        subBuilder.done();
    }

    // Configuration Options

    void setRefreshRatePerSec(double refreshRatePerSec) {
        _rateLimiter.setRefreshRatePerSec(refreshRatePerSec);
    }

    void setBurstSize(double burstSize) {
        _rateLimiter.setBurstSize(burstSize);
    }

    void setMaxQueueDepth(int64_t maxQueueDepth) {
        _rateLimiter.setMaxQueueDepth(maxQueueDepth);
    }

private:
    admission::RateLimiter _rateLimiter;
    decltype(ServerGlobalParams::maxEstablishingConnsOverride)::Snapshot
        _maxEstablishingConnsOverride;

    // Stats
    Counter64 _added;
    Counter64 _removed;
    Counter64 _rejected;
    Counter64 _exempted;
    Counter64 _interuptedDueToClientDisconnect;
};
}  // namespace mongo::transport
