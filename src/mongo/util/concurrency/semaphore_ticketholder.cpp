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
    uint64_t available = _tickets.load();
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
                                                                       Date_t deadline,
                                                                       bool interruptible) {
    boost::optional<Ticket> maybeTicket = _tryAcquireImpl(admCtx);
    if (MONGO_likely(maybeTicket)) {
        if (interruptible) {
            opCtx->checkForInterrupt();
        }

        return std::move(*maybeTicket);
    }

    while (true) {
        Waitable::TimeoutState status;
        _parkingLot.runWithNotifyable(*opCtx->getBaton(), [&]() noexcept {
            ClockSource* clockSource = opCtx->getServiceContext()->getPreciseClockSource();
            Baton* baton = opCtx->getBaton().get();
            status = baton->run_until(clockSource, deadline);
        });

        if (status == Waitable::TimeoutState::Timeout) {
            return boost::none;
        }

        if (interruptible) {
            opCtx->checkForInterrupt();
        }

        if (boost::optional<Ticket> maybeTicket = _tryAcquireImpl(admCtx)) {
            return std::move(*maybeTicket);
        }
    }
}

void SemaphoreTicketHolder::_releaseToTicketPoolImpl(AdmissionContext* admCtx) noexcept {
    _tickets.fetchAndAdd(1);
    _parkingLot.notifyOne();
}

int32_t SemaphoreTicketHolder::available() const {
    return _tickets.load();
}

}  // namespace mongo
