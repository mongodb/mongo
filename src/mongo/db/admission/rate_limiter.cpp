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
#include "mongo/logv2/log_severity_suppressor.h"
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

double calculateBurstSize(double refreshRate, double burstCapacitySecs) {
    return refreshRate * burstCapacitySecs;
}

}  // namespace

class RateLimiter::RateLimiterPrivate {
public:
    RateLimiterPrivate(double r, double b, int64_t m, TickSource* clock, std::string n)
        // Initialize the token bucket with one "burst" of tokens. The third parameter to
        // tokenBucket's constructor ("zeroTime") is interpreted as a number of seconds from the
        // epoch of the clock used by the token bucket. The clock is
        // `std::chrono::steady_clock`, whose epoch is unspecified but is usually the boot time
        // of the machine. Rather than have an initial accumulation of tokens based on some
        // unknown point in the past, set the zero time to a known time in the past: enough time
        // for burst size (b) tokens to have accumulated.
        : maxQueueDepth(m),
          queued(0),
          name(std::move(n)),
          _tickSource(clock),
          _tokenBucket{r, b, nowInSeconds() - b / r} {}

    /*
     * Used to protect all calls into the token bucket that do not require modification of the
     * bucket instance.
     *
     * Functions are thin wrappers around correspondingly-named functions in folly/TokenBucket.h.
     * Please refer to TokenBucket.h for function comments.
     */
    class ReadScopedTokenBucket {
    public:
        ReadScopedTokenBucket(folly::TokenBucket& tb, WriteRarelyRWMutex& m)
            : _l(m.readLock()), _tb(tb) {}

        bool consume(double c, double now) {
            return _tb.consume(c, now);
        }

        boost::optional<double> consumeWithBorrowNonBlocking(double c, double now) {
            return _tb.consumeWithBorrowNonBlocking(c, now);
        }

        double available(double now) {
            return _tb.available(now);
        }

        double balance(double now) {
            return _tb.balance(now);
        }

        void returnTokens(double tk) {
            _tb.returnTokens(tk);
        }

    private:
        WriteRarelyRWMutex::ReadLock _l;
        folly::TokenBucket& _tb;
    };

    /*
     * Used to protect all calls into the token bucket that require modification of the bucket
     * instance.
     *
     * Functions are thin wrappers around correspondingly-named functions in folly/TokenBucket.h.
     * Please refer to TokenBucket.h for function comments.
     */
    class WriteScopedTokenBucket {
    public:
        WriteScopedTokenBucket(folly::TokenBucket& tb, WriteRarelyRWMutex& m)
            : _l(m.writeLock()), _tb(tb) {}

        void reset(double rt, double b) {
            _tb.reset(rt, b);
        }

    private:
        WriteRarelyRWMutex::WriteLock _l;
        folly::TokenBucket& _tb;
    };

    Stats stats;

    Atomic<int64_t> maxQueueDepth;
    Atomic<int64_t> queued;

    std::string name;

    ReadScopedTokenBucket readScopedTokenBucket() {
        return {_tokenBucket, _rwMutex};
    }

    WriteScopedTokenBucket writeScopedTokenBucket() {
        return {_tokenBucket, _rwMutex};
    }

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

    double nowInSeconds() {
        return std::chrono::duration<double>(std::chrono::nanoseconds(_tickSource->getTicks()))
            .count();
    }

private:
    WriteRarelyRWMutex _rwMutex;
    TickSource* _tickSource;
    folly::TokenBucket _tokenBucket;
};

RateLimiter::DeferredToken::DeferredToken(RateLimiterPrivate* impl,
                                          Milliseconds napTime,
                                          double numTokens)
    : _impl(impl), _napTime(napTime), _numTokens(numTokens) {}


RateLimiter::DeferredToken::~DeferredToken() {
    if (!_impl || isReady()) {
        return;
    }
    // Unconsumed non-ready deferred token: return the borrowed token and release the queue slot.
    _impl->readScopedTokenBucket().returnTokens(_numTokens);
    _impl->queued.fetchAndSubtract(1);
    _impl->stats.removedFromQueue.incrementRelaxed();
}

Status RateLimiter::DeferredToken::get(OperationContext* opCtx) && {
    invariant(_impl);

    if (isReady()) {
        // Token was immediately available. No need to update stats, they were already recorded in
        // acquireToken().
        _impl = nullptr;
        return Status::OK();
    }

    auto* impl = std::exchange(_impl, nullptr);

    ON_BLOCK_EXIT([impl] {
        impl->stats.removedFromQueue.incrementRelaxed();
        impl->queued.fetchAndSubtract(1);
    });

    Date_t deadline = opCtx->getServiceContext()->getPreciseClockSource()->now() + _napTime;
    try {
        LOGV2_DEBUG(10550200,
                    4,
                    "Going to sleep waiting for token acquisition",
                    "rateLimiterName"_attr = impl->name,
                    "napTimeMillis"_attr = _napTime.toString());
        opCtx->sleepUntil(deadline);
    } catch (const DBException& e) {
        impl->stats.interruptedInQueue.incrementRelaxed();
        LOGV2_DEBUG(10440800,
                    4,
                    "Interrupted while waiting in rate limiter queue",
                    "rateLimiterName"_attr = impl->name,
                    "exception"_attr = e.toString());
        impl->readScopedTokenBucket().returnTokens(_numTokens);
        return e.toStatus().withContext(fmt::format(
            "Interrupted while waiting in rate limiter queue. rateLimiterName={}", impl->name));
    }

    impl->stats.successfulAdmissions.incrementRelaxed();
    impl->stats.averageTimeQueuedMicros.addSample(
        static_cast<double>(durationCount<Microseconds>(_napTime)));
    return Status::OK();
}

void RateLimiter::DeferredToken::recordExemption() && {
    invariant(_impl);
    invariant(_napTime > Milliseconds{0});  // Exemptions are only supported for queued requests.

    // This method only records the exemption, token/queue cleanup is covered by the destructor.
    _impl->stats.exemptedAdmissions.incrementRelaxed();
}

RateLimiter::RateLimiter(double refreshRatePerSec,
                         double burstCapacitySecs,
                         int64_t maxQueueDepth,
                         std::string name,
                         TickSource* tickSource) {
    uassert(ErrorCodes::InvalidOptions,
            fmt::format("burstCapacitySecs cannot be less than or equal to 0.0. "
                        "burstCapacitySecs={}; rateLimiterName={}",
                        burstCapacitySecs,
                        name),
            burstCapacitySecs > 0.0);
    auto burstSize = calculateBurstSize(refreshRatePerSec, burstCapacitySecs);
    _impl = std::make_unique<RateLimiterPrivate>(
        refreshRatePerSec, burstSize, maxQueueDepth, tickSource, std::move(name));
}

RateLimiter::~RateLimiter() = default;

StatusWith<RateLimiter::DeferredToken> RateLimiter::acquireToken(double numTokensToConsume) {
    const bool hangInLimiter = hangInRateLimiter.shouldFail();
    const auto maxQueueDepth = _impl->maxQueueDepth.loadRelaxed();
    if (!hangInLimiter && (maxQueueDepth <= 0 || _impl->queued.load() >= maxQueueDepth)) {
        // Queueing unavailable (disabled or currently full): use try-acquire semantics.
        if (auto status = tryAcquireToken(numTokensToConsume); !status.isOK()) {
            return status;
        }
        _impl->stats.averageTimeQueuedMicros.addSample(0);
        return DeferredToken(_impl.get(), Milliseconds{0}, numTokensToConsume);
    }

    _impl->stats.attemptedAdmissions.incrementRelaxed();

    double waitForTokenSecs;
    if (hangInLimiter) {
        waitForTokenSecs = 60 * 60;  // 1 hour
    } else {
        waitForTokenSecs =
            _impl->readScopedTokenBucket()
                .consumeWithBorrowNonBlocking(numTokensToConsume, _impl->nowInSeconds())
                .value_or(0);
    }

    auto napTime = doubleToMillis(waitForTokenSecs);
    if (napTime > Milliseconds{0}) {
        // Token not immediately available: reserve a queue slot.
        if (auto status = _impl->enqueue(); !status.isOK()) {
            _impl->readScopedTokenBucket().returnTokens(numTokensToConsume);
            _impl->stats.rejectedAdmissions.incrementRelaxed();
            return status;
        }
        _impl->stats.addedToQueue.incrementRelaxed();
        return DeferredToken(_impl.get(), napTime, numTokensToConsume);
    }

    // Token immediately available.
    _impl->stats.successfulAdmissions.incrementRelaxed();
    _impl->stats.averageTimeQueuedMicros.addSample(0);
    return DeferredToken(_impl.get(), Milliseconds{0}, numTokensToConsume);
}

Status RateLimiter::acquireToken(OperationContext* opCtx, double numTokensToConsume) {
    auto tokenResult = acquireToken(numTokensToConsume);
    if (!tokenResult.isOK()) {
        return tokenResult.getStatus();
    }
    return std::move(tokenResult.getValue()).get(opCtx);
}

Status RateLimiter::tryAcquireToken(double numTokensToConsume) {
    _impl->stats.attemptedAdmissions.incrementRelaxed();

    if (!_impl->readScopedTokenBucket().consume(numTokensToConsume, _impl->nowInSeconds())) {
        _impl->stats.rejectedAdmissions.incrementRelaxed();
        return Status{kRejectedErrorCode,
                      fmt::format("Rate limiter '{}' rate exceeded", _impl->name)};
    }

    _impl->stats.successfulAdmissions.incrementRelaxed();
    return Status::OK();
}

void RateLimiter::returnTokens(double numTokensToReturn) {
    _impl->readScopedTokenBucket().returnTokens(numTokensToReturn);
}

void RateLimiter::recordExemption() {
    _impl->stats.exemptedAdmissions.incrementRelaxed();
}

void RateLimiter::updateRateParameters(double refreshRatePerSec, double burstCapacitySecs) {
    uassert(ErrorCodes::InvalidOptions,
            fmt::format("burstCapacitySecs cannot be less than or equal to 0.0. "
                        "burstCapacitySecs={}; rateLimiterName={}",
                        burstCapacitySecs,
                        _impl->name),
            burstCapacitySecs > 0.0);
    auto burstSize = calculateBurstSize(refreshRatePerSec, burstCapacitySecs);
    _impl->writeScopedTokenBucket().reset(refreshRatePerSec, burstSize);
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

    // FTDC consumers may not handle infinity, and so we append INT64_MAX instead.
    bob->append("totalAvailableTokens",
                static_cast<double>(std::min(static_cast<long double>(INT64_MAX),
                                             static_cast<long double>(tokensAvailable()))));
}

double RateLimiter::tokensAvailable() const {
    return _impl->readScopedTokenBucket().available(_impl->nowInSeconds());
}

double RateLimiter::tokenBalance() const {
    return _impl->readScopedTokenBucket().balance(_impl->nowInSeconds());
}

int64_t RateLimiter::queued() const {
    return _impl->queued.load();
}

int64_t RateLimiter::maxQueueDepth() const {
    return _impl->maxQueueDepth.loadRelaxed();
}

}  // namespace mongo::admission
