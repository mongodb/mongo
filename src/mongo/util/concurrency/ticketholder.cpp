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
#include "mongo/db/service_context.h"
#include "mongo/db/storage/storage_engine_feature_flags_gen.h"
#include "mongo/util/concurrency/admission_context.h"

#include <iostream>

#include "mongo/logv2/log.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {

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

void TicketHolder::resize(int32_t newSize) noexcept {
    stdx::lock_guard<Latch> lk(_resizeMutex);

    _resize(newSize, _outof.load());
    _outof.store(newSize);
}

void TicketHolder::appendStats(BSONObjBuilder& b) const {
    b.append("out", used());
    b.append("available", available());
    b.append("totalTickets", outof());
    b.append("immediatePriorityAdmissionsCount", getImmediatePriorityAdmissionsCount());
    _appendImplStats(b);
}

void TicketHolder::_releaseToTicketPool(AdmissionContext* admCtx) noexcept {
    auto& queueStats = _getQueueStatsToUse(admCtx);
    updateQueueStatsOnRelease(_serviceContext, queueStats, admCtx);
    _releaseToTicketPoolImpl(admCtx);
}

Ticket TicketHolder::waitForTicket(OperationContext* opCtx, AdmissionContext* admCtx) {
    auto res = waitForTicketUntil(opCtx, admCtx, Date_t::max());
    invariant(res);
    return std::move(*res);
}

boost::optional<Ticket> TicketHolder::tryAcquire(AdmissionContext* admCtx) {
    // 'kImmediate' operations should always bypass the ticketing system.
    invariant(admCtx && admCtx->getPriority() != AdmissionContext::Priority::kImmediate);
    auto ticket = _tryAcquireImpl(admCtx);

    if (ticket) {
        auto& queueStats = _getQueueStatsToUse(admCtx);
        updateQueueStatsOnTicketAcquisition(_serviceContext, queueStats, admCtx);
        _updatePeakUsed();
    }
    return ticket;
}


boost::optional<Ticket> TicketHolder::waitForTicketUntil(OperationContext* opCtx,
                                                         AdmissionContext* admCtx,
                                                         Date_t until) {
    invariant(admCtx && admCtx->getPriority() != AdmissionContext::Priority::kImmediate);

    // Attempt a quick acquisition first.
    if (auto ticket = tryAcquire(admCtx)) {
        return ticket;
    }

    auto& queueStats = _getQueueStatsToUse(admCtx);
    auto tickSource = _serviceContext->getTickSource();
    auto currentWaitTime = tickSource->getTicks();
    auto updateQueuedTime = [&]() {
        auto oldWaitTime = std::exchange(currentWaitTime, tickSource->getTicks());
        auto waitDelta = tickSource->ticksTo<Microseconds>(currentWaitTime - oldWaitTime).count();
        queueStats.totalTimeQueuedMicros.fetchAndAddRelaxed(waitDelta);
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

    auto ticket = _waitForTicketUntilImpl(opCtx, admCtx, until);

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
    return _peakUsed.swap(used());
}

void TicketHolder::_updatePeakUsed() {
    // (Ignore FCV check): This feature flag doesn't have upgrade/downgrade concern.
    if (!feature_flags::gFeatureFlagExecutionControl.isEnabledAndIgnoreFCVUnsafe()) {
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

boost::optional<Ticket> MockTicketHolder::tryAcquire(AdmissionContext*) {
    return {};
}

Ticket MockTicketHolder::waitForTicket(OperationContext*, AdmissionContext*) {
    return {nullptr, nullptr};
}

boost::optional<Ticket> MockTicketHolder::waitForTicketUntil(OperationContext*,
                                                             AdmissionContext*,
                                                             Date_t) {
    return {};
}

boost::optional<Ticket> MockTicketHolder::_tryAcquireImpl(AdmissionContext* admCtx) {
    return boost::none;
}

boost::optional<Ticket> MockTicketHolder::_waitForTicketUntilImpl(OperationContext* opCtx,
                                                                  AdmissionContext* admCtx,
                                                                  Date_t until) {
    return boost::none;
}

void MockTicketHolder::setUsed(int32_t used) {
    _used = used;
    if (_used > _peakUsed) {
        _peakUsed = _used;
    }
}

}  // namespace mongo
