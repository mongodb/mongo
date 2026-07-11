// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/admission/ticketing/unordered_ticket_semaphore.h"

#include "mongo/db/operation_context.h"


namespace mongo {

namespace {

Date_t nextDeadline(Date_t until) {
    // Timed waits can be problematic if we have a large number of waiters, since each time we
    // check for interrupt we risk waking up all waiting threads at the same time. We introduce
    // some jitter here to try to reduce the impact of a thundering herd of waiters woken at
    // the same time.
    constexpr auto kBaseInterval = 500;
    constexpr double kJitterFactor = 0.2;
    static thread_local XorShift128 urbg(SecureRandom().nextInt64());
    int32_t offset = std::uniform_int_distribution<int32_t>(-kJitterFactor * kBaseInterval,
                                                            kBaseInterval * kJitterFactor)(urbg);
    return std::min(until, Date_t::now() + Milliseconds{kBaseInterval + offset});
}
}  // namespace

bool UnorderedTicketSemaphore::tryAcquire() {
    int available = _permits.load();

    while (available > 0) {
        if (_permits.compareAndSwap(&available, available - 1)) {
            return true;
        }
    }

    return false;
}

bool UnorderedTicketSemaphore::acquire(OperationContext* opCtx,
                                       AdmissionContext* admCtx,
                                       Date_t until,
                                       bool interruptible) {
    bool hasStartedWaiting = false;

    ON_BLOCK_EXIT([&] {
        if (hasStartedWaiting) {
            _waiters.fetchAndSubtract(1);
        }
    });

    while (true) {
        if (tryAcquire()) {
            return true;
        }

        Date_t deadline = nextDeadline(until);

        if (!hasStartedWaiting) {
            const auto previousWaiters = _waiters.fetchAndAdd(1);
            hasStartedWaiting = true;
            if (previousWaiters >= _maxWaiters.loadRelaxed()) {
                admCtx->recordOperationLoadShed();
                uasserted(
                    ErrorCodes::AdmissionQueueOverflow,
                    "MongoDB is overloaded and cannot accept new operations. Try again later.");
            }
        }

        _permits.waitUntil(0, deadline);

        if (interruptible) {
            opCtx->checkForInterrupt();
        }

        if (deadline == until) {
            if (!interruptible) {
                // Uninterruptible wait - just return timeout
                return false;
            }

            // It's possible that system_clock (used by BasicWaitableAtomic::waitUntil)
            // is slightly ahead of FastClock (used by OperationContext::checkForInterrupt).
            // Handle this clock skew by explicitly checking the deadline against until,
            // similar to OperationContext::waitForConditionOrInterruptNoAssertUntil.
            opCtx->checkForDeadlineExpired(until);

            // We only reach here when 'until' < opCtx->getDeadline(), meaning the caller
            // provided an explicit deadline that is earlier than the operation's maxTimeMS.
            // In this case, we return false to indicate the caller's deadline expired
            // (non-throwing), while the operation itself remains alive.
            return false;
        }
    }

    MONGO_UNREACHABLE;
}

void UnorderedTicketSemaphore::release() {
    // Notifying a futex costs a syscall. Since queued waiters guarantee that the `_waiters` is
    // non-zero while they are waiting, we can avoid the needless cost if there are permits and no
    // queued waiters.
    int availableBeforeIncrementing = _permits.fetchAndAdd(1);
    if (availableBeforeIncrementing >= 0 && _waiters.load() > 0) {
        _permits.notifyOne();
    }
}

void UnorderedTicketSemaphore::resize(int delta) {
    auto oldAvailable = _permits.fetchAndAdd(delta);
    if ((oldAvailable <= 0) && ((oldAvailable + delta) > 0)) {
        _permits.notifyMany(oldAvailable + delta);
    }
}

int UnorderedTicketSemaphore::available() const {
    return _permits.load();
}

void UnorderedTicketSemaphore::setMaxWaiters(int waiters) {
    _maxWaiters.store(waiters);
}

int UnorderedTicketSemaphore::waiters() const {
    return _waiters.load();
}

}  // namespace mongo
