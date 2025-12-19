/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/util/concurrency/ticketholder.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/ticketholder_parameters_gen.h"
#include "mongo/util/duration.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/tick_source.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {

TicketHolder::TicketHolder(ServiceContext* serviceContext,
                           int numTickets,
                           bool trackPeakUsed,
                           std::int32_t maxQueueDepth,
                           DelinquentCallback delinquentCallback,
                           AcquisitionCallback acquisitionCallback,
                           WaitedAcquisitionCallback waitedAcquisitionCallback,
                           ReleaseCallback releaseCallback,
                           ResizePolicy resizePolicy)
    : _trackPeakUsed(trackPeakUsed),
      _resizePolicy(resizePolicy),
      _serviceContext(serviceContext),
      _tickets(numTickets),
      _maxQueueDepth(maxQueueDepth),
      _outof(numTickets),
      _reportDelinquentOpCallback(delinquentCallback),
      _reportAcquisitionOpCallback(acquisitionCallback),
      _reportWaitedAcquisitionOpCallback(waitedAcquisitionCallback),
      _reportReleaseOpCallback(releaseCallback) {
    _enabledDelinquent = gFeatureFlagRecordDelinquentMetrics.isEnabled();
    _delinquentMs = Milliseconds(gDelinquentAcquisitionIntervalMillis.load());
}

bool TicketHolder::resize(OperationContext* opCtx, int32_t newSize, Date_t deadline) {
    stdx::lock_guard<stdx::mutex> lk(_resizeMutex);
    auto difference = newSize - _outof.load();
    MockAdmissionContext admCtx;

    switch (_resizePolicy) {
        case ResizePolicy::kGradual:
            if (difference > 0) {
                // Hand out tickets one-by-one until we've given them all out.
                for (auto remaining = difference; remaining > 0; remaining--) {
                    // This call bypasses statistics reporting.
                    _releaseNormalPriorityTicket(&admCtx);
                    _outof.fetchAndAdd(1);
                }
            } else {
                tassert(10330200,
                        "TicketHolder: OperationContext must be provided for gradual resize",
                        opCtx);
                // Make sure the operation isn't interrupted before waiting for tickets.
                opCtx->checkForInterrupt();
                // Take tickets one-by-one without releasing.
                for (auto remaining = -difference; remaining > 0; remaining--) {
                    // This call bypasses statistics reporting.
                    auto ticket = _performWaitForTicketUntil(opCtx, &admCtx, deadline, true);
                    if (!ticket) {
                        // We timed out getting a ticket, fail the resize.
                        return false;
                    }
                    ticket->discard();
                    _outof.fetchAndSubtract(1);
                }
            }
            return true;
        case ResizePolicy::kImmediate:
            _immediateResize(lk, newSize);
            return true;
    }
    MONGO_UNREACHABLE;
}

void TicketHolder::setMaxQueueDepth(int32_t size) {
    _maxQueueDepth.store(size);
}

void TicketHolder::_immediateResize(WithLock, int32_t newSize) {
    auto oldSize = _outof.swap(newSize);
    auto delta = newSize - oldSize;
    auto oldAvailable = _tickets.fetchAndAdd(delta);
    if ((oldAvailable <= 0) && ((oldAvailable + delta) > 0)) {
        _tickets.notifyMany(oldAvailable + delta);
    }
}

void TicketHolder::_releaseTicketUpdateStats(Ticket& ticket) {
    if (ticket._priority == AdmissionContext::Priority::kExempt) {
        _updateQueueStatsOnRelease(_exemptStats, ticket);
        return;
    }

    ticket._admissionContext->markTicketReleased();

    _updateQueueStatsOnRelease(_holderStats, ticket);
    _releaseNormalPriorityTicket(ticket._admissionContext);
}

Ticket TicketHolder::waitForTicket(OperationContext* opCtx, AdmissionContext* admCtx) {
    auto res = waitForTicketUntil(opCtx, admCtx, Date_t::max());
    invariant(res);
    return std::move(*res);
}

boost::optional<Ticket> TicketHolder::waitForTicketUntil(OperationContext* opCtx,
                                                         AdmissionContext* admCtx,
                                                         Date_t until) {
    return _waitForTicketUntilMaybeInterruptible(opCtx, admCtx, until, true);
}

boost::optional<Ticket> TicketHolder::waitForTicketUntilNoInterrupt_DO_NOT_USE(
    OperationContext* opCtx, AdmissionContext* admCtx, Date_t until) {
    return _waitForTicketUntilMaybeInterruptible(opCtx, admCtx, until, false);
}

boost::optional<Ticket> TicketHolder::_waitForTicketUntilMaybeInterruptible(
    OperationContext* opCtx, AdmissionContext* admCtx, Date_t until, bool interruptible) {
    // Attempt a quick acquisition first.
    if (auto ticket = tryAcquire(admCtx)) {
        return ticket;
    }

    auto tickSource = _serviceContext->getTickSource();
    _holderStats.totalAddedQueue.fetchAndAddRelaxed(1);
    ON_BLOCK_EXIT([&, startWaitTime = tickSource->getTicks()] {
        auto waitDelta = tickSource->ticksTo<Microseconds>(tickSource->getTicks() - startWaitTime);
        _holderStats.totalTimeQueuedMicros.fetchAndAddRelaxed(waitDelta.count());
        _holderStats.totalRemovedQueue.fetchAndAddRelaxed(1);
        if (_reportWaitedAcquisitionOpCallback) {
            _reportWaitedAcquisitionOpCallback(admCtx, waitDelta);
        }
    });

    ScopeGuard cancelWait([&] {
        // Update statistics.
        _holderStats.totalCanceled.fetchAndAddRelaxed(1);
    });

    WaitingForAdmissionGuard waitForAdmission(admCtx, tickSource);
    auto ticket = _performWaitForTicketUntil(opCtx, admCtx, until, interruptible);

    if (ticket) {
        cancelWait.dismiss();
        _updateQueueStatsOnTicketAcquisition(admCtx, _holderStats, admCtx->getPriority());
        _updatePeakUsed();
        return ticket;
    } else {
        return boost::none;
    }
}

boost::optional<Ticket> TicketHolder::_performWaitForTicketUntil(OperationContext* opCtx,
                                                                 AdmissionContext* admCtx,
                                                                 Date_t until,
                                                                 bool interruptible) {
    // Cap 'until' to not exceed the operation's deadline.
    if (interruptible && opCtx->hasDeadline()) {
        until = std::min(until, opCtx->getDeadline());
    }

    auto nextDeadline = [&]() {
        // Timed waits can be problematic if we have a large number of waiters, since each time we
        // check for interrupt we risk waking up all waiting threads at the same time. We introduce
        // some jitter here to try to reduce the impact of a thundering herd of waiters woken at
        // the same time.
        static int32_t baseIntervalMs = 500;
        static double jitterFactor = 0.2;
        static thread_local XorShift128 urbg(SecureRandom().nextInt64());
        int32_t offset = std::uniform_int_distribution<int32_t>(
            -jitterFactor * baseIntervalMs, baseIntervalMs * jitterFactor)(urbg);
        return std::min(until, Date_t::now() + Milliseconds{baseIntervalMs + offset});
    };

    bool hasStartedWaiting = false;

    // Since uassert throws, we use raii to substract the waiter count
    ON_BLOCK_EXIT([&] {
        if (hasStartedWaiting) {
            _waiterCount.fetchAndSubtract(1);
        }
    });

    while (true) {
        if (boost::optional<Ticket> maybeTicket = _tryAcquireNormalPriorityTicket(admCtx);
            maybeTicket) {
            return maybeTicket;
        }

        Date_t deadline = nextDeadline();

        if (!hasStartedWaiting) {
            const auto previousWaiterCount = _waiterCount.fetchAndAdd(1);
            hasStartedWaiting = true;
            if (previousWaiterCount >= _maxQueueDepth.loadRelaxed()) {
                admCtx->recordOperationLoadShed();
                uasserted(
                    ErrorCodes::AdmissionQueueOverflow,
                    "MongoDB is overloaded and cannot accept new operations. Try again later.");
            }
        }

        _tickets.waitUntil(0, deadline);

        if (interruptible) {
            opCtx->checkForInterrupt();
        }

        if (deadline == until) {
            if (!interruptible) {
                // Uninterruptible wait - just return timeout
                return boost::none;
            }

            // It's possible that system_clock (used by BasicWaitableAtomic::waitUntil)
            // is slightly ahead of FastClock (used by OperationContext::checkForInterrupt).
            // Handle this clock skew by explicitly checking the deadline against until,
            // similar to OperationContext::waitForConditionOrInterruptNoAssertUntil.
            opCtx->checkForDeadlineExpired(until);

            // We only reach here when 'until' < opCtx->getDeadline(), meaning the caller
            // provided an explicit deadline that is earlier than the operation's maxTimeMS.
            // In this case, we return boost::none to indicate the caller's deadline expired
            // (non-throwing), while the operation itself remains alive.
            return boost::none;
        }
    }
}

boost::optional<Ticket> TicketHolder::tryAcquire(AdmissionContext* admCtx) {
    const auto currentPriority = admCtx->getPriority();
    if (currentPriority == AdmissionContext::Priority::kExempt) {
        _updateQueueStatsOnTicketAcquisition(admCtx, _exemptStats, currentPriority);
        return Ticket{this, admCtx};
    }

    auto ticket = _tryAcquireNormalPriorityTicket(admCtx);
    if (ticket) {
        _updateQueueStatsOnTicketAcquisition(admCtx, _holderStats, currentPriority);
        _updatePeakUsed();
    }

    return ticket;
}

boost::optional<Ticket> TicketHolder::_tryAcquireNormalPriorityTicket(AdmissionContext* admCtx) {
    int32_t available = _tickets.load();
    while (true) {
        if (available <= 0) {
            return boost::none;
        }

        if (_tickets.compareAndSwap(&available, available - 1)) {
            return _makeTicket(admCtx);
        }
    }
}

int32_t TicketHolder::getAndResetPeakUsed() {
    invariant(_trackPeakUsed);
    return _peakUsed.swap(used());
}

void TicketHolder::_updatePeakUsed() {
    if (!_trackPeakUsed) {
        return;
    }

    auto currentUsed = used();
    auto peakUsed = _peakUsed.load();

    while (currentUsed > peakUsed && !_peakUsed.compareAndSwap(&peakUsed, currentUsed)) {
    }
}

void TicketHolder::appendExemptStats(BSONObjBuilder& b) const {
    _appendQueueStats(b, _exemptStats);
}

void TicketHolder::appendHolderStats(BSONObjBuilder& b) const {
    _appendQueueStats(b, _holderStats);
    _delinquencyStats.appendStats(b);
}

void TicketHolder::appendTicketStats(BSONObjBuilder& b) const {
    b.append("out", used());
    b.append("available", available());
    b.append("totalTickets", outof());
}

void TicketHolder::_appendQueueStats(BSONObjBuilder& b, const QueueStats& stats) const {
    auto removed = stats.totalRemovedQueue.loadRelaxed();
    auto added = stats.totalAddedQueue.loadRelaxed();

    b.append("addedToQueue", added);
    b.append("removedFromQueue", removed);
    b.append("queueLength", std::max(added - removed, (int64_t)0));

    auto finished = stats.totalFinishedProcessing.loadRelaxed();
    auto started = stats.totalStartedProcessing.loadRelaxed();
    b.append("startedProcessing", started);
    b.append("processing", std::max(started - finished, (int64_t)0));
    b.append("finishedProcessing", finished);
    b.append("totalTimeProcessingMicros", stats.totalTimeProcessingMicros.loadRelaxed());
    b.append("canceled", stats.totalCanceled.loadRelaxed());
    b.append("newAdmissions", stats.totalNewAdmissions.loadRelaxed());
    b.append("totalTimeQueuedMicros", stats.totalTimeQueuedMicros.loadRelaxed());
}

void TicketHolder::_updateQueueStatsOnRelease(TicketHolder::QueueStats& queueStats,
                                              const Ticket& ticket) {
    queueStats.totalFinishedProcessing.fetchAndAddRelaxed(1);
    auto tickSource = _serviceContext->getTickSource();
    auto delta =
        tickSource->ticksTo<Microseconds>(tickSource->getTicks() - ticket._acquisitionTime);
    queueStats.totalTimeProcessingMicros.fetchAndAddRelaxed(delta.count());

    if (_enabledDelinquent && _reportDelinquentOpCallback && delta > _delinquentMs) {
        _reportDelinquentOpCallback(ticket.getAdmissionContext(),
                                    duration_cast<Milliseconds>(delta));
    }
    if (_reportReleaseOpCallback) {
        _reportReleaseOpCallback(ticket._admissionContext, duration_cast<Microseconds>(delta));
    }
}

void TicketHolder::_updateQueueStatsOnTicketAcquisition(AdmissionContext* admCtx,
                                                        TicketHolder::QueueStats& queueStats,
                                                        AdmissionContext::Priority priority) {
    // The admission context is shared across both normal and low priority ticket holders, so we
    // need to check the priority-specific counter rather than total admissions.
    bool isNewAdmission = false;
    if (priority == AdmissionContext::Priority::kLow) {
        isNewAdmission = admCtx->getLowAdmissions() == 0;
    } else {
        isNewAdmission = admCtx->getAdmissions() == 0;
    }

    if (isNewAdmission) {
        queueStats.totalNewAdmissions.fetchAndAddRelaxed(1);
    }

    if (priority == AdmissionContext::Priority::kExempt) {
        admCtx->recordExemptedAdmission();
    }

    if (priority == AdmissionContext::Priority::kLow) {
        admCtx->recordLowAdmission();
    }

    admCtx->recordAdmission();
    queueStats.totalStartedProcessing.fetchAndAddRelaxed(1);
    if (_reportAcquisitionOpCallback) {
        _reportAcquisitionOpCallback(admCtx);
    }
}

int64_t TicketHolder::numFinishedProcessing() const {
    return _holderStats.totalFinishedProcessing.load();
}

int64_t TicketHolder::queued() const {
    auto removed = _holderStats.totalRemovedQueue.loadRelaxed();
    auto added = _holderStats.totalAddedQueue.loadRelaxed();
    return std::max(added - removed, (int64_t)0);
};

int32_t TicketHolder::available() const {
    return _tickets.load();
}

void TicketHolder::_releaseNormalPriorityTicket(AdmissionContext* admCtx) {
    // Notifying a futex costs a syscall. Since queued waiters guarantee that the `_waiterCount` is
    // non-zero while they are waiting, we can avoid the needless cost if there are tickets and no
    // queued waiters.
    int32_t availableBeforeIncrementing = _tickets.fetchAndAdd(1);
    if (availableBeforeIncrementing >= 0 && _waiterCount.load() > 0) {
        _tickets.notifyOne();
    }
}

void TicketHolder::setNumFinishedProcessing_forTest(int32_t numFinishedProcessing) {
    _holderStats.totalFinishedProcessing.store(numFinishedProcessing);
}

void TicketHolder::setPeakUsed_forTest(int32_t used) {
    _peakUsed.store(used);
}

int32_t TicketHolder::waiting_forTest() const {
    return _waiterCount.load();
}

void TicketHolder::incrementDelinquencyStats(
    const admission::execution_control::DelinquencyStats& newStats) {
    _delinquencyStats += newStats;
}

}  // namespace mongo
