/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/service_context.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/concurrency/priority_ticketholder.h"

#include <iostream>

#include "mongo/logv2/log.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
PriorityTicketHolder::PriorityTicketHolder(int numTickets,
                                           int lowPriorityBypassThreshold,
                                           ServiceContext* serviceContext)
    : TicketHolderWithQueueingStats(numTickets, serviceContext),
      _lowPriorityBypassThreshold(lowPriorityBypassThreshold),
      _ticketsAvailable(numTickets),
      _serviceContext(serviceContext) {}

void PriorityTicketHolder::updateLowPriorityAdmissionBypassThreshold(
    const int& newBypassThreshold) {
    stdx::unique_lock<stdx::mutex> growthLock(_growthMutex);
    _lowPriorityBypassThreshold = newBypassThreshold;
}

boost::optional<Ticket> PriorityTicketHolder::_tryAcquireImpl(AdmissionContext* admCtx) {
    invariant(admCtx);

    // Handed over tickets to queued waiters do not affect this path since they are not accounted
    // for in the general ticketsAvailable counter.
    auto hasAcquired = _tryAcquireTicket();
    if (hasAcquired) {
        return Ticket{this, admCtx};
    }
    return boost::none;
}

TicketBroker::WaitingResult PriorityTicketHolder::_attemptToAcquireTicket(
    TicketBroker& ticketBroker, Date_t deadline, Milliseconds maxWaitTime) {
    // We are going to enter the broker as a waiter, so we must block releasers momentarily before
    // registering ourselves as a waiter. Otherwise we risk missing a ticket.
    stdx::unique_lock growthLock(_growthMutex);
    // Check if a ticket became present in the general pool. This prevents a potential
    // deadlock if the following were to happen without a tryAcquire:
    // * Thread A proceeds to wait for a ticket to be handed over but before it acquires the
    // growthLock gets descheduled.
    // * Thread B releases a ticket, sees no waiters and releases to the general pool.
    // * Thread A acquires the lock and proceeds to wait.
    //
    // In this scenario Thread A would spin indefinitely since it never picks up that there is a
    // ticket in the general pool. It would wait until another thread comes in and hands over a
    // ticket.
    if (_tryAcquireTicket()) {
        TicketBroker::WaitingResult result;
        result.hasTimedOut = false;
        result.hasTicket = true;
        return result;
    }
    // We wait for a tiny bit before checking for interruption.
    auto maxUntil = std::min(deadline, Date_t::now() + maxWaitTime);
    return ticketBroker.attemptWaitForTicketUntil(std::move(growthLock), maxUntil);
}

boost::optional<Ticket> PriorityTicketHolder::_waitForTicketUntilImpl(OperationContext* opCtx,
                                                                      AdmissionContext* admCtx,
                                                                      Date_t until,
                                                                      WaitMode waitMode) {
    invariant(admCtx);

    auto queueType = _getQueueType(admCtx);
    auto& ticketBroker = _getBroker(queueType);

    bool interruptible = waitMode == WaitMode::kInterruptible;

    while (true) {
        // We attempt to acquire a ticket for a period of time. This may or may not succeed, in
        // which case we will retry until timing out or getting interrupted.
        auto waitingResult = _attemptToAcquireTicket(ticketBroker, until, Milliseconds{500});
        ScopeGuard rereleaseIfTimedOutOrInterrupted([&] {
            // We may have gotten a ticket that we can't use, release it back to the ticket pool.
            if (waitingResult.hasTicket) {
                _releaseToTicketPoolImpl(admCtx);
            }
        });
        if (interruptible) {
            opCtx->checkForInterrupt();
        }
        auto hasTimedOut = waitingResult.hasTimedOut;
        if (hasTimedOut && Date_t::now() > until) {
            return boost::none;
        }
        // We haven't been interrupted or timed out, so we may have a valid ticket present.
        rereleaseIfTimedOutOrInterrupted.dismiss();
        if (waitingResult.hasTicket) {
            return Ticket{this, admCtx};
        }
    }
}

void PriorityTicketHolder::_releaseToTicketPoolImpl(AdmissionContext* admCtx) noexcept {
    // 'Immediate' priority operations should bypass the ticketing system completely.
    invariant(admCtx && admCtx->getPriority() != AdmissionContext::Priority::kImmediate);

    // We will now proceed to perform dequeueing, we must acquire the growth mutex in order to
    // prevent new enqueuers.
    stdx::unique_lock growthLock(_growthMutex);

    auto hasWokenThread = _dequeueWaitingThread(growthLock);
    if (!hasWokenThread) {
        // There's no-one in the queue left to wake, so we give the ticket back for general
        // availability.
        _ticketsAvailable.addAndFetch(1);
    }
}

void PriorityTicketHolder::_resize(OperationContext* opCtx, int newSize, int oldSize) noexcept {
    auto difference = newSize - oldSize;

    if (difference > 0) {
        // As we're adding tickets the waiting threads need to be notified that there are new
        // tickets available.
        stdx::unique_lock dequeuerLock(_growthMutex);
        for (int i = 0; i < difference; i++) {
            auto hasWokenThread = _dequeueWaitingThread(dequeuerLock);
            if (!hasWokenThread) {
                // There's no-one in the brokers left to wake, so we give the ticket back for
                // general availability.
                _ticketsAvailable.addAndFetch(1);
            }
        }
    } else {
        AdmissionContext admCtx;
        for (int i = 0; i < std::abs(difference); i++) {
            // This operation is uninterruptible as the resize operation is conceptually atomic.
            // Cancelling the resize and leaving it in-between the old size and the new one is not
            // allowed.
            auto ticket = _waitForTicketUntilImpl(
                opCtx, &admCtx, Date_t::max(), TicketHolder::WaitMode::kUninterruptible);
            invariant(ticket);
            ticket->discard();
        }
    }
}

TicketHolderWithQueueingStats::QueueStats& PriorityTicketHolder::_getQueueStatsToUse(
    const AdmissionContext* admCtx) noexcept {
    auto queueType = _getQueueType(admCtx);
    return _stats[_enumToInt(queueType)];
}

void PriorityTicketHolder::_appendImplStats(BSONObjBuilder& b) const {
    {
        BSONObjBuilder bbb(b.subobjStart("lowPriority"));
        const auto& lowPriorityTicketStats = _stats[_enumToInt(QueueType::kLowPriority)];
        _appendCommonQueueImplStats(bbb, lowPriorityTicketStats);
        bbb.append("expedited", expedited());
        bbb.append("bypassed", bypassed());
        bbb.done();
    }
    {
        BSONObjBuilder bbb(b.subobjStart("normalPriority"));
        const auto& normalPriorityTicketStats = _stats[_enumToInt(QueueType::kNormalPriority)];
        _appendCommonQueueImplStats(bbb, normalPriorityTicketStats);
        bbb.done();
    }
    b.append("immediatePriorityAdmissionsCount", getImmediatePriorityAdmissionsCount());
}

bool PriorityTicketHolder::_tryAcquireTicket() {
    // Test, then test and set to avoid invalidating a cache line unncessarily.
    if (_ticketsAvailable.loadRelaxed() <= 0) {
        return false;
    }
    auto remaining = _ticketsAvailable.subtractAndFetch(1);
    if (remaining < 0) {
        _ticketsAvailable.addAndFetch(1);
        return false;
    }
    return true;
}

bool PriorityTicketHolder::_dequeueWaitingThread(const stdx::unique_lock<stdx::mutex>& growthLock) {
    // There are only 2 possible brokers to transfer our ticket to - the low priority and normal
    // priority brokers. There will never be anything to transfer to the immediate priority broker,
    // given immediate priority operations will never wait for ticket admission.
    auto& lowPriorityBroker = _getBroker(QueueType::kLowPriority);
    auto& normalPriorityBroker = _getBroker(QueueType::kNormalPriority);

    // There is a guarantee that the number of waiters will not increase while holding the growth
    // lock. This check is safe as long as we only compare it against an upper bound.
    auto lowPrioWaiting = lowPriorityBroker.waitingThreadsRelaxed();
    auto normalPrioWaiting = normalPriorityBroker.waitingThreadsRelaxed();

    if (lowPrioWaiting == 0 && normalPrioWaiting == 0) {
        return false;
    }
    if (lowPrioWaiting == 0) {
        return normalPriorityBroker.attemptToTransferTicket(growthLock);
    }
    if (normalPrioWaiting == 0) {
        return lowPriorityBroker.attemptToTransferTicket(growthLock);
    }

    // Both brokers are non-empty, and the low priority broker is bypassed for release in favor of
    // the normal priority broker until the bypass threshold is met.
    if (_lowPriorityBypassThreshold > 0 &&
        _lowPriorityBypassCount.addAndFetch(1) % _lowPriorityBypassThreshold == 0) {
        if (lowPriorityBroker.attemptToTransferTicket(growthLock)) {
            _expeditedLowPriorityAdmissions.addAndFetch(1);
            return true;
        } else {
            return normalPriorityBroker.attemptToTransferTicket(growthLock);
        }
    }

    if (!normalPriorityBroker.attemptToTransferTicket(growthLock)) {
        return lowPriorityBroker.attemptToTransferTicket(growthLock);
    } else {
        return true;
    }
}

}  // namespace mongo
