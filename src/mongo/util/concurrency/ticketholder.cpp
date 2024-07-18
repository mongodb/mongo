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
#include "mongo/util/duration.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/tick_source.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {

void TicketHolder::_updateQueueStatsOnRelease(TicketHolder::QueueStats& queueStats,
                                              const Ticket& ticket) {
    queueStats.totalFinishedProcessing.fetchAndAddRelaxed(1);
    auto tickSource = _serviceContext->getTickSource();
    auto delta =
        tickSource->ticksTo<Microseconds>(tickSource->getTicks() - ticket._acquisitionTime);
    queueStats.totalTimeProcessingMicros.fetchAndAddRelaxed(delta.count());
}

void TicketHolder::_updateQueueStatsOnTicketAcquisition(AdmissionContext* admCtx,
                                                        TicketHolder::QueueStats& queueStats) {
    if (admCtx->getAdmissions() == 0) {
        queueStats.totalNewAdmissions.fetchAndAddRelaxed(1);
    }
    admCtx->recordAdmission();
    queueStats.totalStartedProcessing.fetchAndAddRelaxed(1);
}

TicketHolder::TicketHolder(ServiceContext* svcCtx, int32_t numTickets, bool trackPeakUsed)
    : _trackPeakUsed(trackPeakUsed), _outof(numTickets), _serviceContext(svcCtx) {}

bool TicketHolder::resize(OperationContext* opCtx, int32_t newSize, Date_t deadline) {
    stdx::lock_guard<Latch> lk(_resizeMutex);
    return _resizeImpl(lk, opCtx, newSize, deadline);
}

bool TicketHolder::_resizeImpl(WithLock,
                               OperationContext* opCtx,
                               int32_t newSize,
                               Date_t deadline) {
    auto difference = newSize - _outof.load();
    MockAdmissionContext admCtx;
    if (difference > 0) {
        // Hand out tickets one-by-one until we've given them all out.
        for (auto remaining = difference; remaining > 0; remaining--) {
            // This call bypasses statistics reporting.
            _releaseToTicketPoolImpl(&admCtx);
            _outof.fetchAndAdd(1);
        }
    } else {
        // Make sure the operation isn't interrupted before waiting for tickets.
        opCtx->checkForInterrupt();
        // Take tickets one-by-one without releasing.
        for (auto remaining = -difference; remaining > 0; remaining--) {
            // This call bypasses statistics reporting.
            auto ticket = _waitForTicketUntilImpl(opCtx, &admCtx, deadline, true);
            if (!ticket) {
                // We timed out getting a ticket, fail the resize.
                return false;
            }
            ticket->discard();
            _outof.fetchAndSubtract(1);
        }
    }
    return true;
}

void TicketHolder::appendStats(BSONObjBuilder& b) const {
    b.append("out", used());
    b.append("available", available());
    b.append("totalTickets", outof());
    {
        BSONObjBuilder bb(b.subobjStart("exempt"));
        _appendCommonQueueImplStats(bb, _exemptQueueStats);
        bb.done();
    }

    _appendImplStats(b);
}

void TicketHolder::_releaseToTicketPool(Ticket& ticket) noexcept {
    if (ticket._priority == AdmissionContext::Priority::kExempt) {
        _updateQueueStatsOnRelease(_exemptQueueStats, ticket);
        return;
    }

    ticket._admissionContext->markTicketReleased();

    auto& queueStats = _getQueueStatsToUse(ticket._priority);
    _updateQueueStatsOnRelease(queueStats, ticket);
    _releaseToTicketPoolImpl(ticket._admissionContext);
}

Ticket TicketHolder::waitForTicket(OperationContext* opCtx, AdmissionContext* admCtx) {
    auto res = waitForTicketUntil(opCtx, admCtx, Date_t::max());
    invariant(res);
    return std::move(*res);
}

boost::optional<Ticket> TicketHolder::tryAcquire(AdmissionContext* admCtx) {
    if (admCtx->getPriority() == AdmissionContext::Priority::kExempt) {
        _updateQueueStatsOnTicketAcquisition(admCtx, _exemptQueueStats);
        return Ticket{this, admCtx};
    }

    auto ticket = _tryAcquireImpl(admCtx);
    if (ticket) {
        auto& queueStats = _getQueueStatsToUse(admCtx->getPriority());
        _updateQueueStatsOnTicketAcquisition(admCtx, queueStats);
        _updatePeakUsed();
    }

    return ticket;
}

boost::optional<Ticket> TicketHolder::_waitForTicketUntil(OperationContext* opCtx,
                                                          AdmissionContext* admCtx,
                                                          Date_t until,
                                                          bool interruptible) {
    // Attempt a quick acquisition first.
    if (auto ticket = tryAcquire(admCtx)) {
        return ticket;
    }

    auto& queueStats = _getQueueStatsToUse(admCtx->getPriority());
    auto tickSource = _serviceContext->getTickSource();
    queueStats.totalAddedQueue.fetchAndAddRelaxed(1);
    ON_BLOCK_EXIT([&, startWaitTime = tickSource->getTicks()] {
        auto waitDelta = tickSource->ticksTo<Microseconds>(tickSource->getTicks() - startWaitTime);
        queueStats.totalTimeQueuedMicros.fetchAndAddRelaxed(waitDelta.count());
        queueStats.totalRemovedQueue.fetchAndAddRelaxed(1);
    });

    ScopeGuard cancelWait([&] {
        // Update statistics.
        queueStats.totalCanceled.fetchAndAddRelaxed(1);
    });

    WaitingForAdmissionGuard waitForAdmission(admCtx, _serviceContext->getTickSource());
    auto ticket = _waitForTicketUntilImpl(opCtx, admCtx, until, interruptible);

    if (ticket) {
        cancelWait.dismiss();
        _updateQueueStatsOnTicketAcquisition(admCtx, queueStats);
        _updatePeakUsed();
        return ticket;
    } else {
        return boost::none;
    }
}

boost::optional<Ticket> TicketHolder::waitForTicketUntil(OperationContext* opCtx,
                                                         AdmissionContext* admCtx,
                                                         Date_t until) {
    return _waitForTicketUntil(opCtx, admCtx, until, true);
}

boost::optional<Ticket> TicketHolder::waitForTicketUntilNoInterrupt_DO_NOT_USE(
    OperationContext* opCtx, AdmissionContext* admCtx, Date_t until) {
    return _waitForTicketUntil(opCtx, admCtx, until, false);
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

void TicketHolder::_appendCommonQueueImplStats(BSONObjBuilder& b, const QueueStats& stats) const {
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

void MockTicketHolder::_releaseToTicketPoolImpl(AdmissionContext* admCtx) noexcept {
    _available++;
}

boost::optional<Ticket> MockTicketHolder::_tryAcquireImpl(AdmissionContext* admCtx) {
    if (_available <= 0) {
        return boost::none;
    }
    _available--;
    if (used() > _peakUsed.load()) {
        _peakUsed.store(used());
    }
    return _makeTicket(admCtx);
}

boost::optional<Ticket> MockTicketHolder::_waitForTicketUntilImpl(OperationContext* opCtx,
                                                                  AdmissionContext* admCtx,
                                                                  Date_t until,
                                                                  bool interruptible) {
    return _tryAcquireImpl(admCtx);
}
}  // namespace mongo
