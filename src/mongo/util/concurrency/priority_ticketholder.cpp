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
      _serviceContext(serviceContext) {
    _ticketsAvailable.store(numTickets);
    _enqueuedElements.store(0);
}

void PriorityTicketHolder::updateLowPriorityAdmissionBypassThreshold(
    const int& newBypassThreshold) {
    ticket_queues::UniqueLockGuard uniqueQueueLock(_queueMutex);
    _lowPriorityBypassThreshold = newBypassThreshold;
}

boost::optional<Ticket> PriorityTicketHolder::_tryAcquireImpl(AdmissionContext* admCtx) {
    invariant(admCtx);
    // Low priority operations cannot use optimistic ticket acquisition and will go to the queue
    // instead. This is done to prevent them from skipping the line before other high-priority
    // operations.
    if (admCtx->getPriority() >= AdmissionContext::Priority::kNormal) {
        auto hasAcquired = _tryAcquireTicket();
        if (hasAcquired) {
            return Ticket{this, admCtx};
        }
    }
    return boost::none;
}

boost::optional<Ticket> PriorityTicketHolder::_waitForTicketUntilImpl(OperationContext* opCtx,
                                                                      AdmissionContext* admCtx,
                                                                      Date_t until,
                                                                      WaitMode waitMode) {
    invariant(admCtx);

    auto queueType = _getQueueType(admCtx);
    auto& queue = _getQueue(queueType);

    bool interruptible = waitMode == WaitMode::kInterruptible;

    _enqueuedElements.addAndFetch(1);
    ON_BLOCK_EXIT([&] { _enqueuedElements.subtractAndFetch(1); });

    ticket_queues::UniqueLockGuard uniqueQueueLock(_queueMutex);
    do {
        while (_ticketsAvailable.load() <= 0 ||
               _hasToWaitForHigherPriority(uniqueQueueLock, queueType)) {
            bool hasTimedOut = !queue.enqueue(uniqueQueueLock, opCtx, until, interruptible);
            if (hasTimedOut) {
                return boost::none;
            }
        }
    } while (!_tryAcquireTicket());
    return Ticket{this, admCtx};
}

void PriorityTicketHolder::_releaseToTicketPoolImpl(AdmissionContext* admCtx) noexcept {
    // Tickets acquired with priority kImmediate are not generated from the pool of available
    // tickets, and thus should never be returned to the pool of available tickets.
    invariant(admCtx && admCtx->getPriority() != AdmissionContext::Priority::kImmediate);

    // The idea behind the release mechanism consists of a consistent view of queued elements
    // waiting for a ticket and many threads releasing tickets simultaneously. The releasers will
    // proceed to attempt to dequeue an element by seeing if there are threads not woken and waking
    // one, having increased the number of woken threads for accuracy. Once the thread gets woken it
    // will then decrease the number of woken threads (as it has been woken) and then attempt to
    // acquire a ticket. The two possible states are either one or more releasers releasing or a
    // thread waking up due to the RW mutex.
    //
    // Under this lock the queues cannot be modified in terms of someone attempting to enqueue on
    // them, only waking threads is allowed.
    ticket_queues::SharedLockGuard sharedQueueLock(_queueMutex);
    _ticketsAvailable.addAndFetch(1);
    _dequeueWaitingThread(sharedQueueLock);
}

void PriorityTicketHolder::_resize(int newSize, int oldSize) noexcept {
    auto difference = newSize - oldSize;

    _ticketsAvailable.fetchAndAdd(difference);

    if (difference > 0) {
        // As we're adding tickets the waiting threads need to be notified that there are new
        // tickets available.
        ticket_queues::SharedLockGuard sharedQueueLock(_queueMutex);
        for (int i = 0; i < difference; i++) {
            _dequeueWaitingThread(sharedQueueLock);
        }
    }

    // No need to do anything in the other cases as the number of tickets being <= 0 implies they'll
    // have to wait until the current ticket holders release their tickets.
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
    {
        BSONObjBuilder bbb(b.subobjStart("immediatePriority"));
        // Since 'kImmediate' priority operations will never queue, omit queueing statistics that
        // will always be 0.
        const auto& immediateTicketStats = _stats[_enumToInt(QueueType::kImmediatePriority)];

        auto finished = immediateTicketStats.totalFinishedProcessing.loadRelaxed();
        auto started = immediateTicketStats.totalStartedProcessing.loadRelaxed();
        bbb.append("startedProcessing", started);
        bbb.append("processing", std::max(static_cast<int>(started - finished), 0));
        bbb.append("finishedProcessing", finished);
        bbb.append("totalTimeProcessingMicros",
                   immediateTicketStats.totalTimeProcessingMicros.loadRelaxed());
        bbb.append("newAdmissions", immediateTicketStats.totalNewAdmissions.loadRelaxed());
        bbb.done();
    }
}

bool PriorityTicketHolder::_tryAcquireTicket() {
    auto remaining = _ticketsAvailable.subtractAndFetch(1);
    if (remaining < 0) {
        _ticketsAvailable.addAndFetch(1);
        return false;
    }
    return true;
}

void PriorityTicketHolder::_dequeueWaitingThread(
    const ticket_queues::SharedLockGuard& sharedQueueLock) {
    // There are only 2 possible queues to dequeue from - the low priority and normal priority
    // queues. There will never be anything to dequeue from the immediate priority queue, given
    // immediate priority operations will never wait for ticket admission.
    auto& lowPriorityQueue = _getQueue(QueueType::kLowPriority);
    auto& normalPriorityQueue = _getQueue(QueueType::kNormalPriority);

    // There is a guarantee that the number of queued elements cannot change while holding the
    // shared queue lock.
    auto lowQueueCount = lowPriorityQueue.queuedElems();
    auto normalQueueCount = normalPriorityQueue.queuedElems();

    if (lowQueueCount == 0 && normalQueueCount == 0) {
        return;
    }
    if (lowQueueCount == 0) {
        normalPriorityQueue.attemptToDequeue(sharedQueueLock);
        return;
    }
    if (normalQueueCount == 0) {
        lowPriorityQueue.attemptToDequeue(sharedQueueLock);
        return;
    }

    // Both queues are non-empty, and the low priority queue is bypassed for dequeue in favor of the
    // normal priority queue until the bypass threshold is met.
    if (_lowPriorityBypassThreshold > 0 &&
        _lowPriorityBypassCount.addAndFetch(1) % _lowPriorityBypassThreshold == 0) {
        if (lowPriorityQueue.attemptToDequeue(sharedQueueLock)) {
            _expeditedLowPriorityAdmissions.addAndFetch(1);
        } else {
            normalPriorityQueue.attemptToDequeue(sharedQueueLock);
        }
        return;
    }

    if (!normalPriorityQueue.attemptToDequeue(sharedQueueLock)) {
        lowPriorityQueue.attemptToDequeue(sharedQueueLock);
    }
}

bool PriorityTicketHolder::_hasToWaitForHigherPriority(const ticket_queues::UniqueLockGuard& lk,
                                                       QueueType queue) {
    switch (queue) {
        case QueueType::kLowPriority: {
            const auto& normalQueue = _getQueue(QueueType::kNormalPriority);
            auto pending = normalQueue.getThreadsPendingToWake();
            return pending != 0 && pending >= _ticketsAvailable.load();
        }
        default:
            return false;
    }
}
}  // namespace mongo
