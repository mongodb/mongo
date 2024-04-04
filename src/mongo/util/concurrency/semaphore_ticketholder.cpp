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

#include "mongo/util/concurrency/semaphore_ticketholder.h"

#include "mongo/db/operation_context.h"

namespace mongo {

int64_t SemaphoreTicketHolder::numFinishedProcessing() const {
    return _semaphoreStats.totalFinishedProcessing.load();
}

void SemaphoreTicketHolder::_appendImplStats(BSONObjBuilder& b) const {
    {
        BSONObjBuilder bb(b.subobjStart("normalPriority"));
        _appendCommonQueueImplStats(bb, _semaphoreStats);
        bb.done();
    }
}

SemaphoreTicketHolder::SemaphoreTicketHolder(ServiceContext* serviceContext,
                                             int numTickets,
                                             bool trackPeakUsed)
    : TicketHolder(serviceContext, numTickets, trackPeakUsed), _tickets(numTickets) {}

boost::optional<Ticket> SemaphoreTicketHolder::_tryAcquireImpl(AdmissionContext* admCtx) {
    uint32_t available = _tickets.load();
    while (true) {
        if (available == 0) {
            return boost::none;
        }

        if (_tickets.compareAndSwap(&available, available - 1)) {
            return Ticket{this, admCtx};
        }
    }
}

boost::optional<Ticket> SemaphoreTicketHolder::_waitForTicketUntilImpl(OperationContext* opCtx,
                                                                       AdmissionContext* admCtx,
                                                                       Date_t until,
                                                                       bool interruptible) {
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

    Date_t deadline = nextDeadline();
    while (true) {
        while (!_tickets.waitUntil(0, deadline)) {
            if (deadline == until) {
                return boost::none;
            }

            deadline = nextDeadline();
            if (interruptible) {
                opCtx->checkForInterrupt();
            }
        }

        uint32_t available = _tickets.load();
        if (available > 0 && _tickets.compareAndSwap(&available, available - 1)) {
            Ticket ticket{this, admCtx};

            if (interruptible) {
                opCtx->checkForInterrupt();
            }

            return std::move(ticket);
        }
    }
}

void SemaphoreTicketHolder::_releaseToTicketPoolImpl(AdmissionContext* admCtx) noexcept {
    _tickets.fetchAndAdd(1);
    _tickets.notifyOne();
}

int32_t SemaphoreTicketHolder::available() const {
    return _tickets.load();
}

}  // namespace mongo
