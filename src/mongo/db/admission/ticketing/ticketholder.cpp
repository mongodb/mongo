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

#include "mongo/db/admission/ticketing/ticketholder.h"

#include "mongo/db/admission/ticketing/ordered_ticket_semaphore.h"
#include "mongo/db/admission/ticketing/ticketholder_parameters_gen.h"
#include "mongo/db/admission/ticketing/unordered_ticket_semaphore.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/tick_source.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {

TicketHolder::TicketHolder(ServiceContext* serviceContext,
                           int numTickets,
                           bool trackPeakUsed,
                           int maxQueueDepth,
                           DelinquentCallback delinquentCallback,
                           AcquisitionCallback acquisitionCallback,
                           WaitedAcquisitionCallback waitedAcquisitionCallback,
                           ReleaseCallback releaseCallback,
                           ResizePolicy resizePolicy,
                           SemaphoreType semaphore)
    : _trackPeakUsed(trackPeakUsed),
      _resizePolicy(resizePolicy),
      _tickSource(serviceContext->getTickSource()),
      _outof(numTickets),
      _reportDelinquentOpCallback(delinquentCallback),
      _reportAcquisitionOpCallback(acquisitionCallback),
      _reportWaitedAcquisitionOpCallback(waitedAcquisitionCallback),
      _reportReleaseOpCallback(releaseCallback) {
    switch (semaphore) {
        case SemaphoreType::kCompeting:
            _semaphore = std::make_unique<UnorderedTicketSemaphore>(numTickets, maxQueueDepth);
            break;
        case SemaphoreType::kPrioritizeFewestAdmissions:
            _semaphore = std::make_unique<OrderedTicketSemaphore>(numTickets, maxQueueDepth);
            break;
    }
    _enabledDelinquent = gFeatureFlagRecordDelinquentMetrics.isEnabled();
    _delinquentMs = Milliseconds(gDelinquentAcquisitionIntervalMillis.load());
}

bool TicketHolder::resize(OperationContext* opCtx, int newSize, Date_t deadline) {
    std::lock_guard<std::mutex> lk(_resizeMutex);
    auto difference = newSize - _outof.load();
    MockAdmissionContext admCtx;

    switch (_resizePolicy) {
        case ResizePolicy::kGradual:
            if (difference > 0) {
                // Hand out tickets one-by-one until we've given them all out.
                for (auto remaining = difference; remaining > 0; remaining--) {
                    // This call bypasses statistics reporting.
                    _semaphore->release();
                    _outof.fetchAndAdd(1);
                }
            } else {
                tassert(10330200,
                        "TicketHolder: OperationContext must be provided for gradual resize",
                        opCtx);
                // Make sure the operation isn't interrupted before waiting for tickets.
                opCtx->checkForInterrupt();

                auto effectiveDeadline =
                    opCtx->hasDeadline() ? std::min(deadline, opCtx->getDeadline()) : deadline;

                // Take tickets one-by-one without releasing.
                for (auto remaining = -difference; remaining > 0; remaining--) {
                    if (!_semaphore->acquire(
                            opCtx, &admCtx, effectiveDeadline, true /* interruptible */)) {
                        // We timed out getting a ticket, fail the resize.
                        return false;
                    }

                    // Reducing capacity does not require issuing a ticket. The semaphore has
                    // already admitted the operation via waitForAdmission, and we simply remove it
                    // from the pool.

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

void TicketHolder::setMaxQueueDepth(int size) {
    _semaphore->setMaxWaiters(size);
}

void TicketHolder::_immediateResize(WithLock, int newSize) {
    auto oldSize = _outof.swap(newSize);
    auto delta = newSize - oldSize;
    _semaphore->resize(delta);
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
    if (admCtx->getPriority() == AdmissionContext::Priority::kExempt) {
        return _issueTicket(admCtx);
    }

    if (_semaphore->tryAcquire()) {
        return _issueTicket(admCtx);
    }

    auto startWaitTime = _tickSource->getTicks();
    _holderStats.totalAddedQueue.fetchAndAddRelaxed(1);

    ON_BLOCK_EXIT([&] {
        auto waitDelta =
            _tickSource->ticksTo<Microseconds>(_tickSource->getTicks() - startWaitTime);

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

    // Cap 'until' to not exceed the operation's deadline.
    if (interruptible && opCtx->hasDeadline()) {
        until = std::min(until, opCtx->getDeadline());
    }

    WaitingForAdmissionGuard waitForAdmission(admCtx, _tickSource);

    if (_semaphore->acquire(opCtx, admCtx, until, interruptible)) {
        cancelWait.dismiss();
        return _issueTicket(admCtx);
    }

    return boost::none;
}

boost::optional<Ticket> TicketHolder::tryAcquire(AdmissionContext* admCtx) {
    if (admCtx->getPriority() == AdmissionContext::Priority::kExempt) {
        return _issueTicket(admCtx);
    }

    if (_semaphore->tryAcquire()) {
        return _issueTicket(admCtx);
    }

    return boost::none;
}

int TicketHolder::getAndResetPeakUsed() {
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

Ticket TicketHolder::_issueTicket(AdmissionContext* admCtx) {
    if (admCtx->getPriority() != AdmissionContext::Priority::kExempt) {
        admCtx->markTicketHeld();
    }

    _recordAcquisition(admCtx, admCtx->getPriority());

    return Ticket{this, admCtx};
}

void TicketHolder::_releaseTicket(Ticket& ticket) {
    auto delta =
        _tickSource->ticksTo<Microseconds>(_tickSource->getTicks() - ticket._acquisitionTime);

    auto& queueStats =
        ticket._priority == AdmissionContext::Priority::kExempt ? _exemptStats : _holderStats;
    queueStats.totalFinishedProcessing.fetchAndAddRelaxed(1);
    queueStats.totalTimeProcessingMicros.fetchAndAddRelaxed(delta.count());

    if (_enabledDelinquent && _reportDelinquentOpCallback && delta > _delinquentMs) {
        _reportDelinquentOpCallback(ticket.getAdmissionContext(),
                                    duration_cast<Milliseconds>(delta));
    }

    if (_reportReleaseOpCallback) {
        _reportReleaseOpCallback(ticket._admissionContext, duration_cast<Microseconds>(delta));
    }

    if (ticket._priority == AdmissionContext::Priority::kExempt) {
        return;
    }

    ticket._admissionContext->markTicketReleased();
    _semaphore->release();
}

void TicketHolder::_recordAcquisition(AdmissionContext* admCtx,
                                      AdmissionContext::Priority priority) {
    // The admission context is shared across both normal and low priority ticket holders, so we
    // need to check the priority-specific counter rather than total admissions.
    bool isNewAdmission = false;
    if (priority == AdmissionContext::Priority::kLow) {
        isNewAdmission = admCtx->getLowAdmissions() == 0;
    } else {
        isNewAdmission = admCtx->getAdmissions() == 0;
    }

    auto& queueStats =
        priority == AdmissionContext::Priority::kExempt ? _exemptStats : _holderStats;

    if (isNewAdmission) {
        queueStats.totalNewAdmissions.fetchAndAddRelaxed(1);
    }
    queueStats.totalStartedProcessing.fetchAndAddRelaxed(1);

    if (priority == AdmissionContext::Priority::kExempt) {
        admCtx->recordExemptedAdmission();
    }

    if (priority == AdmissionContext::Priority::kLow) {
        admCtx->recordLowAdmission();
    }

    admCtx->recordAdmission();

    if (priority != AdmissionContext::Priority::kExempt) {
        _updatePeakUsed();
    }

    if (_reportAcquisitionOpCallback) {
        _reportAcquisitionOpCallback(admCtx, priority);
    }
}

int64_t TicketHolder::numFinishedProcessing() const {
    return _holderStats.totalFinishedProcessing.load();
}

int TicketHolder::queued() const {
    return _semaphore->waiters();
}

int TicketHolder::available() const {
    return _semaphore->available();
}

void TicketHolder::setNumFinishedProcessing_forTest(int64_t numFinishedProcessing) {
    _holderStats.totalFinishedProcessing.store(numFinishedProcessing);
}

void TicketHolder::setPeakUsed_forTest(int used) {
    _peakUsed.store(used);
}

void TicketHolder::incrementDelinquencyStats(
    const admission::execution_control::DelinquencyStats& newStats) {
    _delinquencyStats += newStats;
}

}  // namespace mongo
