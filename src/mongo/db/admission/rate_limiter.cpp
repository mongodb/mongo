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

#include "mongo/logv2/log.h"
#include "mongo/util/duration.h"
#include "mongo/util/scopeguard.h"

#include <folly/TokenBucket.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo::admission {
MONGO_FAIL_POINT_DEFINE(hangInRateLimiter);

namespace {
Milliseconds doubleToMillis(double t) {
    return duration_cast<Milliseconds>(
        duration_cast<std::chrono::milliseconds>(std::chrono::duration<double, std::ratio<1>>(t)));
}
}  // namespace

struct RateLimiter::RateLimiterPrivate {
    RateLimiterPrivate(double r, double b, int64_t m, std::string n)
        // Initialize the token bucket with one "burst" of tokens. The third parameter to
        // tokenBucket's constructor ("zeroTime") is interpreted as a number of seconds from the
        // epoch of the clock used by the token bucket. The clock is
        // `std::chrono::steady_clock`, whose epoch is unspecified but is usually the boot time
        // of the machine. Rather than have an initial accumulation of tokens based on some
        // unknown point in the past, set the zero time to a known time in the past: enough time
        // for burst size (b) tokens to have accumulated.
        : tokenBucket{r, b, folly::TokenBucket::defaultClockNow() - b / r},
          maxQueueDepth(m),
          queued(0),
          name(std::move(n)) {}

    WriteRarelyRWMutex rwMutex;
    folly::TokenBucket tokenBucket;

    Stats stats;

    Atomic<int64_t> maxQueueDepth;
    Atomic<int64_t> queued;

    std::string name;

    Status enqueue() {
        const auto maxDepth = maxQueueDepth.loadRelaxed();
        int64_t expected = queued.load();
        do {
            if (expected >= maxDepth) {
                return Status(kRejectedErrorCode,
                              fmt::format("Rate limiter '{}' maximum queue depth ({}) exceeded",
                                          name,
                                          maxDepth));
            }
        } while (!queued.compareAndSwap(&expected, expected + 1));

        return Status::OK();
    }
};

RateLimiter::RateLimiter(double refreshRatePerSec,
                         double burstSize,
                         int64_t maxQueueDepth,
                         std::string name) {
    uassert(ErrorCodes::InvalidOptions,
            fmt::format("burstSize cannot be less than 1.0. burstSize={}; rateLimiterName={}",
                        burstSize,
                        name),
            burstSize >= 1.0);
    _impl = std::make_unique<RateLimiterPrivate>(
        refreshRatePerSec, burstSize, maxQueueDepth, std::move(name));
}

RateLimiter::~RateLimiter() = default;

Status RateLimiter::acquireToken(OperationContext* opCtx) {
    _impl->stats.attemptedAdmissions.incrementRelaxed();

    // The consumeWithBorrowNonBlocking API consumes a token (possibly leading to a negative
    // bucket balance), and returns how long the consumer should nap until their token
    // reservation becomes valid.
    double waitForTokenSecs;
    if (MONGO_unlikely(hangInRateLimiter.shouldFail())) {
        waitForTokenSecs = 60 * 60;  // 1 hour
    } else {
        auto lk = _impl->rwMutex.readLock();
        waitForTokenSecs = _impl->tokenBucket.consumeWithBorrowNonBlocking(1.0).value_or(0);
    }

    if (auto napTime = doubleToMillis(waitForTokenSecs); napTime > Milliseconds{0}) {
        // Calculate the deadline before incrementing the queued metric to ensure that unit tests
        // don't advance the mock clock before the sleep deadline is calculated.
        Date_t deadline = opCtx->getServiceContext()->getPreciseClockSource()->now() + napTime;
        if (auto status = _impl->enqueue(); !status.isOK()) {
            _impl->stats.rejectedAdmissions.incrementRelaxed();
            return status;
        }
        _impl->stats.addedToQueue.incrementRelaxed();
        ON_BLOCK_EXIT([&] {
            _impl->stats.removedFromQueue.incrementRelaxed();
            _impl->queued.fetchAndSubtract(1);
        });
        try {
            LOGV2_DEBUG(10550200,
                        4,
                        "Going to sleep waiting for token acquisition",
                        "rateLimiterName"_attr = _impl->name,
                        "napTimeMillis"_attr = napTime.toString());
            opCtx->sleepUntil(deadline);
        } catch (const DBException& e) {
            _impl->stats.interruptedInQueue.incrementRelaxed();
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

    _impl->stats.successfulAdmissions.incrementRelaxed();
    _impl->stats.averageTimeQueuedMicros.addSample(waitForTokenSecs * 1'000'000);
    return Status::OK();
}

void RateLimiter::recordExemption() {
    _impl->stats.exemptedAdmissions.incrementRelaxed();
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

const RateLimiter::Stats& RateLimiter::stats() const {
    return _impl->stats;
}

void RateLimiter::appendStats(BSONObjBuilder* bob) const {
    invariant(bob);
    bob->append("addedToQueue", stats().addedToQueue.get());
    bob->append("removedFromQueue", stats().removedFromQueue.get());
    bob->append("interruptedInQueue", stats().interruptedInQueue.get());
    bob->append("rejectedAdmissions", stats().rejectedAdmissions.get());
    bob->append("exemptedAdmissions", stats().exemptedAdmissions.get());
    bob->append("successfulAdmissions", stats().successfulAdmissions.get());
    bob->append("attemptedAdmissions", stats().attemptedAdmissions.get());
    if (const auto avg = stats().averageTimeQueuedMicros.get()) {
        bob->append("averageTimeQueuedMicros", *avg);
    }
    bob->append("totalAvailableTokens", tokensAvailable());
}

double RateLimiter::tokensAvailable() const {
    auto lk = _impl->rwMutex.readLock();
    return _impl->tokenBucket.available();
}

int64_t RateLimiter::queued() const {
    return _impl->queued.load();
}

}  // namespace mongo::admission
