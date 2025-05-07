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

#include "mongo/db/admission/rate_limiter.h"

#include <folly/TokenBucket.h>

#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo::admission {
namespace {
Milliseconds doubleToMillis(double t) {
    return duration_cast<Milliseconds>(
        duration_cast<std::chrono::milliseconds>(std::chrono::duration<double, std::ratio<1>>(t)));
}
}  // namespace

struct RateLimiter::RateLimiterPrivate {
    RateLimiterPrivate(
        double r, double b, int64_t m, std::string n, std::unique_ptr<RateLimiter::Stats> s)
        : tokenBucket{r, b}, maxQueueDepth(m), name(std::move(n)), stats(std::move(s)) {}

    WriteRarelyRWMutex rwMutex;
    folly::TokenBucket tokenBucket;

    Atomic<int64_t> maxQueueDepth;
    Atomic<int64_t> numWaiters;

    std::string name;

    /**
     * Users may inherit from the RateLimiter::Stats class and define their own stats to track.
     */
    std::unique_ptr<Stats> stats;

    Status rejectIfOverQueueLimit(double nWaiters) {
        auto maxDepth = maxQueueDepth.loadRelaxed();
        if (MONGO_unlikely(nWaiters >= maxDepth)) {
            {
                auto lk = rwMutex.readLock();
                tokenBucket.returnTokens(1.0);
            }
            return Status(ErrorCodes::TemporarilyUnavailable,
                          fmt::format("RateLimiter queue depth has exceeded the maxQueueDepth. "
                                      "numWaiters={}; maxQueueDepth={}; rateLimiterName={}",
                                      nWaiters,
                                      maxDepth,
                                      name));
        }

        return Status::OK();
    }
};

RateLimiter::RateLimiter(double refreshRatePerSec,
                         double burstSize,
                         int64_t maxQueueDepth,
                         std::string name,
                         std::unique_ptr<Stats> stats) {
    uassert(ErrorCodes::InvalidOptions,
            fmt::format("burstSize cannot be less than 1.0. burstSize={}; rateLimiterName={}",
                        burstSize,
                        name),
            burstSize >= 1.0);
    _impl = std::make_unique<RateLimiterPrivate>(
        refreshRatePerSec, burstSize, maxQueueDepth, std::move(name), std::move(stats));
}

RateLimiter::~RateLimiter() = default;

Status RateLimiter::acquireToken(OperationContext* opCtx) {
    // The consumeWithBorrowNonBlocking API consumes a token (possibly leading to a negative
    // bucket balance), and returns how long the consumer should nap until their token
    // reservation becomes valid.
    double waitForTokenSecs;
    {
        auto lk = _impl->rwMutex.readLock();
        waitForTokenSecs = _impl->tokenBucket.consumeWithBorrowNonBlocking(1.0).value_or(0);
    }

    if (auto napTime = doubleToMillis(waitForTokenSecs); napTime > Milliseconds{0}) {
        auto nWaiters = _impl->numWaiters.fetchAndAdd(1);
        ON_BLOCK_EXIT([&] { _impl->numWaiters.fetchAndSubtract(1); });
        if (auto s = _impl->rejectIfOverQueueLimit(nWaiters); !s.isOK()) {
            return s;
        }

        try {
            opCtx->sleepFor(napTime);
        } catch (const DBException& e) {
            LOGV2_DEBUG(10440800,
                        4,
                        "Interrupted while waiting in rate limiter queue",
                        "rateLimiterName"_attr = _impl->name,
                        "exception"_attr = e.toString());
            {
                auto lk = _impl->rwMutex.readLock();
                _impl->tokenBucket.returnTokens(1.0);
            }
            return e.toStatus().withContext(
                fmt::format("Interrupted while waiting in rate limiter queue. rateLimiterName={}",
                            _impl->name));
        }
    }
    return Status::OK();
}

RateLimiter::Stats* RateLimiter::getRateLimiterStats() {
    return _impl->stats.get();
}

void RateLimiter::setRefreshRatePerSec(double refreshRatePerSec) {
    auto lk = _impl->rwMutex.writeLock();
    _impl->tokenBucket.reset(refreshRatePerSec, _impl->tokenBucket.burst());
}

void RateLimiter::setBurstSize(double burstSize) {
    uassert(ErrorCodes::InvalidOptions,
            fmt::format("burstSize cannot be less than 1.0. burstSize={}; rateLimiterName={}",
                        burstSize,
                        _impl->name),
            burstSize >= 1.0);

    auto lk = _impl->rwMutex.writeLock();
    _impl->tokenBucket.reset(_impl->tokenBucket.rate(), burstSize);
}

void RateLimiter::setMaxQueueDepth(int64_t maxQueueDepth) {
    _impl->maxQueueDepth.storeRelaxed(maxQueueDepth);
}
}  // namespace mongo::admission
