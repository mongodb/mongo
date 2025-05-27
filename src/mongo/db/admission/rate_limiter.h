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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/operation_context.h"

#include <limits>

namespace mongo::admission {

/**
 * The RateLimiter offers a thin wrapper around the folly::TokenBucket augmented with
 * interruptibility, maximum queue depth, and metrics tracking.
 */
class RateLimiter {
public:
    class Stats {
    public:
        void appendStats(BSONObjBuilder&) {
            // TODO: SERVER-104413
        }
    };

    RateLimiter(double refreshRatePerSec,
                double burstSize,
                int64_t maxQueueDepth,
                std::string name = "",
                std::unique_ptr<Stats> stats = std::make_unique<Stats>());

    ~RateLimiter();

    /**
     * Acquire a token or block until one becomes available. Returns an error status if
     * the operationContext is interrupted or the maxQueueDepth is exceeded.
     */
    Status acquireToken(OperationContext*);

    Stats* getRateLimiterStats();

    int64_t getNumWaiters();

    /**
     * The number of tokens issued per second when rate-limiting has kicked in. Tokens will be
     * issued smoothly, rather than all at once every 1 second.
     */
    void setRefreshRatePerSec(double refreshRatePerSec);

    /**
     * The maximum number of tokens that will be issued before rate-limiting kicks in (ie, the
     * maximum number of tokens that can accumulate in the bucket).
     */
    void setBurstSize(double burstSize);

    /**
     * The maximum number of requests enqueued waiting for a token. Token requests that come in and
     * will queue past the maxQueueDepth will be rejected with a TemporarilyUnavailable error.
     */
    void setMaxQueueDepth(int64_t maxQueueDepth);

private:
    struct RateLimiterPrivate;
    std::unique_ptr<RateLimiterPrivate> _impl;
};
}  // namespace mongo::admission
