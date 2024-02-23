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

#include <algorithm>
#include <boost/none.hpp>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/logv2/log.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/interruptible.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/tick_source.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangTicketRelease);

namespace {
void updateQueueStatsOnRelease(ServiceContext* serviceContext,
                               TicketHolder::QueueStats& queueStats,
                               AdmissionContext* admCtx) {
    queueStats.totalFinishedProcessing.fetchAndAddRelaxed(1);
    auto startTime = admCtx->getStartProcessingTime();
    auto tickSource = serviceContext->getTickSource();
    auto delta = tickSource->ticksTo<Microseconds>(tickSource->getTicks() - startTime);
    queueStats.totalTimeProcessingMicros.fetchAndAddRelaxed(delta.count());
}

void updateQueueStatsOnTicketAcquisition(ServiceContext* serviceContext,
                                         TicketHolder::QueueStats& queueStats,
                                         AdmissionContext* admCtx) {
    if (admCtx->getAdmissions() == 0) {
        queueStats.totalNewAdmissions.fetchAndAddRelaxed(1);
    }
    admCtx->start(serviceContext->getTickSource());
    queueStats.totalStartedProcessing.fetchAndAddRelaxed(1);
}
}  // namespace

TicketHolder::TicketHolder(ServiceContext* svcCtx, int32_t numTickets, bool trackPeakUsed)
    : _trackPeakUsed(trackPeakUsed), _outof(numTickets), _serviceContext(svcCtx) {}

bool TicketHolder::resize(int32_t newSize, Date_t deadline) noexcept {
    stdx::lock_guard<Latch> lk(_resizeMutex);

    auto difference = newSize - _outof.load();
    AdmissionContext admCtx;
    if (difference > 0) {
        // Hand out tickets one-by-one until we've given them all out.
        for (auto remaining = difference; remaining > 0; remaining--) {
            // This call bypasses statistics reporting.
            _releaseToTicketPoolImpl(&admCtx);
            _outof.fetchAndAdd(1);
        }
    } else {
        // Take tickets one-by-one without releasing.
        for (auto remaining = -difference; remaining > 0; remaining--) {
            // This call bypasses statistics reporting.
            auto ticket =
                _waitForTicketUntilImpl(*Interruptible::notInterruptible(), &admCtx, deadline);
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
    b.append("immediatePriorityAdmissionsCount", _immediatePriorityAdmissionsCount.loadRelaxed());
    _appendImplStats(b);
}

void TicketHolder::_releaseToTicketPool(AdmissionContext* admCtx) noexcept {
    if (admCtx->getPriority() == AdmissionContext::Priority::kImmediate) {
        return;
    }

    if (MONGO_unlikely(hangTicketRelease.shouldFail())) {
        LOGV2(8435300,
              "Hanging hangTicketRelease in _releaseToTicketPool() due to 'hangTicketRelease' "
              "failpoint");
        hangTicketRelease.pauseWhileSet();
    }

    auto& queueStats = _getQueueStatsToUse(admCtx);
    updateQueueStatsOnRelease(_serviceContext, queueStats, admCtx);
    _releaseToTicketPoolImpl(admCtx);
}

Ticket TicketHolder::waitForTicket(Interruptible& interruptible,
                                   AdmissionContext* admCtx,
                                   Microseconds& timeQueuedForTicketMicros) {
    auto res = waitForTicketUntil(interruptible, admCtx, Date_t::max(), timeQueuedForTicketMicros);
    invariant(res);
    return std::move(*res);
}

boost::optional<Ticket> TicketHolder::tryAcquire(AdmissionContext* admCtx) {
    if (admCtx->getPriority() == AdmissionContext::Priority::kImmediate) {
        _immediatePriorityAdmissionsCount.fetchAndAdd(1);
        return Ticket{this, admCtx};
    }

    auto ticket = _tryAcquireImpl(admCtx);
    if (ticket) {
        auto& queueStats = _getQueueStatsToUse(admCtx);
        updateQueueStatsOnTicketAcquisition(_serviceContext, queueStats, admCtx);
        _updatePeakUsed();
    }

    return ticket;
}

boost::optional<Ticket> TicketHolder::waitForTicketUntil(Interruptible& interruptible,
                                                         AdmissionContext* admCtx,
                                                         Date_t until,
                                                         Microseconds& timeQueuedForTicketMicros) {
    // Attempt a quick acquisition first.
    if (auto ticket = tryAcquire(admCtx)) {
        return ticket;
    }

    auto& queueStats = _getQueueStatsToUse(admCtx);
    auto tickSource = _serviceContext->getTickSource();
    auto currentWaitTime = tickSource->getTicks();
    auto updateQueuedTime = [&]() {
        auto oldWaitTime = std::exchange(currentWaitTime, tickSource->getTicks());
        auto waitDelta = tickSource->ticksTo<Microseconds>(currentWaitTime - oldWaitTime);
        timeQueuedForTicketMicros += waitDelta;
        queueStats.totalTimeQueuedMicros.fetchAndAddRelaxed(waitDelta.count());
    };
    queueStats.totalAddedQueue.fetchAndAddRelaxed(1);
    ON_BLOCK_EXIT([&] {
        updateQueuedTime();
        queueStats.totalRemovedQueue.fetchAndAddRelaxed(1);
    });

    ScopeGuard cancelWait([&] {
        // Update statistics.
        queueStats.totalCanceled.fetchAndAddRelaxed(1);
    });

    auto ticket = _waitForTicketUntilImpl(interruptible, admCtx, until);

    if (ticket) {
        cancelWait.dismiss();
        updateQueueStatsOnTicketAcquisition(_serviceContext, queueStats, admCtx);
        _updatePeakUsed();
        return ticket;
    } else {
        return boost::none;
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

boost::optional<Ticket> MockTicketHolder::tryAcquire(AdmissionContext* admCtx) {
    if (_available <= 0) {
        return boost::none;
    }
    _available--;
    if (used() > _peakUsed.load()) {
        _peakUsed.store(used());
    }
    return Ticket{this, admCtx};
}

Ticket MockTicketHolder::waitForTicket(Interruptible& interruptible,
                                       AdmissionContext* admCtx,
                                       Microseconds& timeQueuedForTicketMicros) {
    auto ticket = tryAcquire(admCtx);
    invariant(ticket);
    return std::move(ticket.get());
}

boost::optional<Ticket> MockTicketHolder::waitForTicketUntil(
    Interruptible& interruptible,
    AdmissionContext* admCtx,
    Date_t until,
    Microseconds& timeQueuedForTicketMicros) {
    return tryAcquire(admCtx);
}

boost::optional<Ticket> MockTicketHolder::_tryAcquireImpl(AdmissionContext* admCtx) {
    return tryAcquire(admCtx);
}

boost::optional<Ticket> MockTicketHolder::_waitForTicketUntilImpl(Interruptible&,
                                                                  AdmissionContext* admCtx,
                                                                  Date_t until) {
    return tryAcquire(admCtx);
}
}  // namespace mongo
