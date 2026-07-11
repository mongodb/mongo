// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/admission/rate_limiter.h"

#include "mongo/logv2/log.h"
#include "mongo/util/duration.h"
#include "mongo/util/scopeguard.h"

#include <utility>

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
    RateLimiterPrivate(double r,
                       double b,
                       int64_t m,
                       TickSource* clock,
                       std::string n,
                       std::unique_ptr<RateLimiterMetricsRecorder> recorder)
        // Initialize the token bucket with one "burst" of tokens. The third parameter to
        // tokenBucket's constructor ("zeroTime") is interpreted as a number of seconds from the
        // epoch of the clock used by the token bucket. The clock is
        // `std::chrono::steady_clock`, whose epoch is unspecified but is usually the boot time
        // of the machine. Rather than have an initial accumulation of tokens based on some
        // unknown point in the past, set the zero time to a known time in the past: enough time
        // for burst size (b) tokens to have accumulated.
        : metricsRecorder{std::move(recorder)},
          maxQueueDepth(m),
          queued(0),
          name(std::move(n)),
          rejectedStatus(kRejectedErrorCode, fmt::format("Rate limiter '{}' rate exceeded", name)),
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

        double rate() {
            return _tb.rate();
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

        void reset(double rt, double b, double now) {
            _tb.reset(rt, b, now);
        }

    private:
        WriteRarelyRWMutex::WriteLock _l;
        folly::TokenBucket& _tb;
    };

    std::unique_ptr<RateLimiterMetricsRecorder> metricsRecorder;

    Atomic<int64_t> maxQueueDepth;
    Atomic<int64_t> queued;

    std::string name;

    // Cached rejection Status so the hot rejection path returns it without a per-call ErrorInfo
    // allocation while still preserving the limiter name in diagnostics.
    const Status rejectedStatus;

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

    Milliseconds nowInMillis() {
        return _tickSource->ticksTo<Milliseconds>(_tickSource->getTicks());
    }

    // Clamped snapshot of available tokens. FTDC consumers may not handle infinity, so the value is
    // capped at INT64_MAX.
    double sampledAvailableTokens() {
        return static_cast<double>(
            std::min(static_cast<long double>(INT64_MAX),
                     static_cast<long double>(readScopedTokenBucket().available(nowInSeconds()))));
    }

private:
    WriteRarelyRWMutex _rwMutex;
    TickSource* _tickSource;
    folly::TokenBucket _tokenBucket;
};

RateLimiter::DeferredToken::DeferredToken(RateLimiterPrivate* impl,
                                          double numTokens,
                                          Milliseconds timeEnqueued,
                                          Milliseconds napTime)
    : _impl(impl), _numTokens(numTokens), _timeEnqueued(timeEnqueued), _napTime(napTime) {}


RateLimiter::DeferredToken::~DeferredToken() {
    if (!_impl || isReady()) {
        return;
    }
    // Unconsumed non-ready deferred token: return the borrowed token and release the queue slot.
    _impl->readScopedTokenBucket().returnTokens(_numTokens);
    _impl->queued.fetchAndSubtract(1);
    _impl->metricsRecorder->record(RemovedFromQueue{});
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
        impl->metricsRecorder->record(RemovedFromQueue{});
        impl->queued.fetchAndSubtract(1);
    });

    // The system can wait arbitrarily long between acquiring a deferred token and calling get() on
    // it, so calculate an adjusted nap time before the actual sleep.
    auto adjustedNapTime =
        std::max(Milliseconds{0}, (_timeEnqueued + _napTime) - impl->nowInMillis());
    if (adjustedNapTime == Milliseconds{0}) {
        impl->metricsRecorder->record(SuccessfulAdmission{_numTokens});
        impl->metricsRecorder->record(
            AverageTimeQueuedMicros{static_cast<double>(durationCount<Microseconds>(_napTime))});
        return Status::OK();
    }

    Date_t deadline = opCtx->getServiceContext()->getPreciseClockSource()->now() + adjustedNapTime;
    try {
        LOGV2_DEBUG(10550200,
                    4,
                    "Going to sleep waiting for token acquisition",
                    "rateLimiterName"_attr = impl->name,
                    "napTimeMillis"_attr = adjustedNapTime.toString());
        opCtx->sleepUntil(deadline);
    } catch (const DBException& e) {
        impl->metricsRecorder->record(InterruptedInQueue{});
        LOGV2_DEBUG(10440800,
                    4,
                    "Interrupted while waiting in rate limiter queue",
                    "rateLimiterName"_attr = impl->name,
                    "exception"_attr = e.toString());
        impl->readScopedTokenBucket().returnTokens(_numTokens);
        return e.toStatus().withContext(fmt::format(
            "Interrupted while waiting in rate limiter queue. rateLimiterName={}", impl->name));
    }

    impl->metricsRecorder->record(SuccessfulAdmission{_numTokens});
    impl->metricsRecorder->record(
        AverageTimeQueuedMicros{static_cast<double>(durationCount<Microseconds>(_napTime))});
    return Status::OK();
}

void RateLimiter::DeferredToken::recordExemption() && {
    invariant(_impl);
    invariant(!isReady());  // Exemptions are only supported for queued requests.

    // This method only records the exemption, token/queue cleanup is covered by the destructor.
    _impl->metricsRecorder->record(ExemptedAdmission{});
}

RateLimiter::RateLimiter(double refreshRatePerSec,
                         double burstCapacitySecs,
                         int64_t maxQueueDepth,
                         std::string name,
                         TickSource* tickSource)
    : RateLimiter(refreshRatePerSec,
                  burstCapacitySecs,
                  maxQueueDepth,
                  std::move(name),
                  RateLimiter::Options{.tickSource = tickSource}) {}

RateLimiter::RateLimiter(double refreshRatePerSec,
                         double burstCapacitySecs,
                         int64_t maxQueueDepth,
                         std::string name,
                         Options options) {
    uassert(ErrorCodes::InvalidOptions,
            fmt::format("burstCapacitySecs cannot be less than or equal to 0.0. "
                        "burstCapacitySecs={}; rateLimiterName={}",
                        burstCapacitySecs,
                        name),
            burstCapacitySecs > 0.0);
    auto burstSize = calculateBurstSize(refreshRatePerSec, burstCapacitySecs);
    _impl = std::make_unique<RateLimiterPrivate>(refreshRatePerSec,
                                                 burstSize,
                                                 maxQueueDepth,
                                                 options.tickSource,
                                                 std::move(name),
                                                 std::move(options.metricsRecorder));
}


RateLimiter::~RateLimiter() = default;

boost::optional<RateLimiter::DeferredToken> RateLimiter::acquireToken(double numTokensToConsume) {
    const bool hangInLimiter = hangInRateLimiter.shouldFail();
    const auto maxQueueDepth = _impl->maxQueueDepth.loadRelaxed();
    if (!hangInLimiter && (maxQueueDepth <= 0 || _impl->queued.load() >= maxQueueDepth)) {
        // Queueing unavailable (disabled or currently full): use try-acquire semantics.
        if (!tryAcquireToken(numTokensToConsume)) {
            return boost::none;
        }
        _impl->metricsRecorder->record(AverageTimeQueuedMicros{0});
        return DeferredToken(_impl.get(), numTokensToConsume, Milliseconds{0}, Milliseconds{0});
    }

    _impl->metricsRecorder->record(AttemptedAdmission{});

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
            _impl->metricsRecorder->record(RejectedAdmission{});
            return boost::none;
        }
        _impl->metricsRecorder->record(AddedToQueue{});
        return DeferredToken(_impl.get(), numTokensToConsume, _impl->nowInMillis(), napTime);
    }

    // Token immediately available.
    _impl->metricsRecorder->record(SuccessfulAdmission{numTokensToConsume});
    _impl->metricsRecorder->record(AverageTimeQueuedMicros{0});
    return DeferredToken(_impl.get(), numTokensToConsume, Milliseconds{0}, Milliseconds{0});
}

Status RateLimiter::acquireToken(OperationContext* opCtx, double numTokensToConsume) {
    auto tokenResult = acquireToken(numTokensToConsume);
    if (!tokenResult) {
        return _impl->rejectedStatus;
    }
    return std::move(*tokenResult).get(opCtx);
}

bool RateLimiter::tryAcquireToken(double numTokensToConsume) {
    _impl->metricsRecorder->record(AttemptedAdmission{});

    if (!_impl->readScopedTokenBucket().consume(numTokensToConsume, _impl->nowInSeconds())) {
        _impl->metricsRecorder->record(RejectedAdmission{});
        return false;
    }
    _impl->metricsRecorder->record(SuccessfulAdmission{numTokensToConsume});
    return true;
}

void RateLimiter::returnTokens(double numTokensToReturn) {
    _impl->readScopedTokenBucket().returnTokens(numTokensToReturn);
}

void RateLimiter::reconcileTokens(double numTokens) {
    if (numTokens <= 0.0) {
        return;
    }
    // Borrow-consume: drains the bucket immediately, allowing the balance to go negative. The
    // returned wait time is intentionally discarded; the reconciliation's effect is felt by the
    // next caller.
    _impl->readScopedTokenBucket().consumeWithBorrowNonBlocking(numTokens, _impl->nowInSeconds());
}

void RateLimiter::recordExemption() {
    _impl->metricsRecorder->record(ExemptedAdmission{});
}

void RateLimiter::updateRateParameters(double refreshRatePerSec, double burstCapacitySecs) {
    uassert(ErrorCodes::InvalidOptions,
            fmt::format("burstCapacitySecs cannot be less than or equal to 0.0. "
                        "burstCapacitySecs={}; rateLimiterName={}",
                        burstCapacitySecs,
                        _impl->name),
            burstCapacitySecs > 0.0);
    auto burstSize = calculateBurstSize(refreshRatePerSec, burstCapacitySecs);
    _impl->writeScopedTokenBucket().reset(refreshRatePerSec, burstSize, _impl->nowInSeconds());
}

void RateLimiter::setMaxQueueDepth(int64_t maxQueueDepth) {
    _impl->maxQueueDepth.storeRelaxed(maxQueueDepth);
}

const RateLimiterMetricsRecorder& RateLimiter::stats() const {
    return *_impl->metricsRecorder;
}

RateLimiterMetricsRecorder& RateLimiter::stats() {
    return *_impl->metricsRecorder;
}

void RateLimiter::appendStats(BSONObjBuilder* bob) const {
    invariant(bob);
    const auto& recorder = *_impl->metricsRecorder;
    bob->append("addedToQueue", recorder.addedToQueue());
    bob->append("removedFromQueue", recorder.removedFromQueue());
    bob->append("interruptedInQueue", recorder.interruptedInQueue());
    bob->append("rejectedAdmissions", recorder.rejectedAdmissions());
    bob->append("exemptedAdmissions", recorder.exemptedAdmissions());
    bob->append("successfulAdmissions", recorder.successfulAdmissions());
    bob->append("attemptedAdmissions", recorder.attemptedAdmissions());
    if (const auto avg = recorder.averageTimeQueuedMicros()) {
        bob->append("averageTimeQueuedMicros", *avg);
    }

    bob->append("tokensAcquired", recorder.tokensAcquired());
    bob->append("currentQueueDepth", recorder.addedToQueue() - recorder.removedFromQueue());

    const auto sampledAvailableTokens = _impl->sampledAvailableTokens();
    _impl->metricsRecorder->record(TokensAvailable{sampledAvailableTokens});
    bob->append("totalAvailableTokens", sampledAvailableTokens);
}

double RateLimiter::refreshRate() const {
    return _impl->readScopedTokenBucket().rate();
}

double RateLimiter::tokensAvailable() const {
    return _impl->readScopedTokenBucket().available(_impl->nowInSeconds());
}

double RateLimiter::sampledAvailableTokens() const {
    return _impl->sampledAvailableTokens();
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
