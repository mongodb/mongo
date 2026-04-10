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
#pragma once

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/admission/execution_control/execution_control_stats.h"
#include "mongo/db/admission/ticketing/admission_context.h"
#include "mongo/db/admission/ticketing/ticket_semaphore.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/modules.h"
#include "mongo/util/tick_source.h"
#include "mongo/util/time_support.h"

#include <cstdint>

#include <boost/optional/optional.hpp>

namespace mongo {

class Ticket;

/**
 * Concurrent admission control mechanism that distributes a finite number of tickets to limit how
 * many operations may proceed simultaneously. Operations acquire a ticket before performing work
 * and release it when done/yield. If tickets are exhausted, incoming operations block until one
 * becomes available.
 *
 * Additionally, it tracks queue and processing statistics (wait times, queue depth, cancellations,
 * peak usage) and fires observer callbacks on acquisition, release, and delinquent operations.
 */
class MONGO_MOD_PUBLIC TicketHolder {
    friend class Ticket;

public:
    using DelinquentCallback = std::function<void(AdmissionContext*, Milliseconds)>;
    using AcquisitionCallback = std::function<void(AdmissionContext*, AdmissionContext::Priority)>;
    using WaitedAcquisitionCallback = std::function<void(AdmissionContext*, Microseconds)>;
    using ReleaseCallback = std::function<void(AdmissionContext*, Microseconds)>;

    /**
     * Describes the algorithm used to update the TicketHolder when the size of the ticket pool
     * changes.
     *
     * kImmediate: update the atomic value _tickets and call notifyMany on waiters if the
     * number of available tickets becomes positive. Otherwise, no waiters are notified.
     *
     * kGradual: iteratively increment _tickets and notify a single waiter for each new
     * available ticket. If the ticket pool shrinks, we iteratively retire the necessary number of
     * tickets as operations finish running.
     */
    enum class ResizePolicy { kGradual = 0, kImmediate };

    /**
     * Specifies Type of semaphore to be used by the ticketing system for a specific ticket pool.
     *
     * kCompeting: uses an unordered semaphore where waiters compete among themself in order to get
     * a ticket.
     * kPrioritizeFewestAdmissions: uses ordered semaphore where waiters are sorted by
     * number of admissions.
     */
    enum class SemaphoreType { kCompeting = 0, kPrioritizeFewestAdmissions };

    /**
     * The default value for maxQueueDepth. It it set to the default max connection amount, which is
     * practically infinite for the purpose of the ticket holder.
     */
    static constexpr int kDefaultMaxQueueDepth = static_cast<int>(DEFAULT_MAX_CONN);

    TicketHolder(ServiceContext* serviceContext,
                 int numTickets,
                 bool trackPeakUsed,
                 int maxQueueDepth,
                 DelinquentCallback delinquentCallback = nullptr,
                 AcquisitionCallback acquisitionCallback = nullptr,
                 WaitedAcquisitionCallback waitedAcquisitionCallback = nullptr,
                 ReleaseCallback releaseCallback = nullptr,
                 ResizePolicy resizePolicy = ResizePolicy::kGradual,
                 SemaphoreType semaphore = SemaphoreType::kCompeting);

    /**
     * Adjusts the total number of tickets allocated for the ticket pool to 'newSize'.
     *
     * Returns 'true' if the resize completed without reaching the 'deadline', and 'false'
     * otherwise.
     */
    bool resize(OperationContext* opCtx, int newSize, Date_t deadline = Date_t::max());

    /**
     * Adjusts the maximum number of threads waiting for a ticket. Will not affect threads already
     * waiting
     */
    void setMaxQueueDepth(int newSize);

    /**
     * Attempts to acquire a ticket without blocking. Returns a ticket if one is available,
     * and boost::none otherwise.
     *
     * Operations exempt from ticketing get issued a new ticket immediately, while normal priority
     * operations take a ticket from the pool if available.
     */
    boost::optional<Ticket> tryAcquire(AdmissionContext* admCtx);

    /**
     * Attempts to acquire a ticket. Blocks until a ticket is acquired or the OperationContext is
     * interrupted, throwing an AssertionException.
     */
    Ticket waitForTicket(OperationContext* opCtx, AdmissionContext* admCtx);

    /**
     * Attempts to acquire a ticket within a deadline, 'until'. Returns 'true' if a ticket is
     * acquired and 'false' if the deadline is reached, but the operation is retryable. Throws an
     * AssertionException if the OperationContext is interrupted.
     */
    boost::optional<Ticket> waitForTicketUntil(OperationContext* opCtx,
                                               AdmissionContext* admCtx,
                                               Date_t until);

    /**
     * The same as `waitForTicketUntil` except the wait will be uninterruptible. Please
     * make every effort to make your waiter interruptible, and try not to use this function! It
     * only exists as a stepping stone until we can complete the work in SERVER-68868 to ensure all
     * work in the server is interruptible.
     *
     * TODO(SERVER-68868): Remove this function completely
     */
    boost::optional<Ticket> waitForTicketUntilNoInterrupt_DO_NOT_USE(OperationContext* opCtx,
                                                                     AdmissionContext* admCtx,
                                                                     Date_t until);

    /**
     * The total number of tickets allotted to the ticket pool.
     */
    int outof() const {
        return _outof.loadRelaxed();
    }

    /**
     * Instantaneous number of tickets that are checked out by an operation.
     */
    int used() const {
        return outof() - available();
    }

    /**
     * Instantaneous number of operations waiting in queue for a ticket.
     */
    int queued() const;

    /**
     * Peak number of tickets checked out at once since the previous time this function was called.
     * Invariants that 'trackPeakUsed' has been passed to the TicketHolder,
     */
    int getAndResetPeakUsed();

    /**
     * Instantaneous number of tickets 'available' (not checked out by an operation) in the ticket
     * pool.
     */
    int available() const;

    /**
     * The total number of operations that acquired a ticket, completed their work, and released the
     * ticket.
     */
    int64_t numFinishedProcessing() const;

    MONGO_MOD_PRIVATE void setNumFinishedProcessing_forTest(int64_t numFinishedProcessing);

    MONGO_MOD_PRIVATE void setPeakUsed_forTest(int used);

    /**
     * Appends all queue and delinquency stats.
     */
    void appendHolderStats(BSONObjBuilder& b) const;

    /**
     * Appends number of tickets available, out and total.
     */
    void appendTicketStats(BSONObjBuilder& b) const;

    /**
     * Append TicketHolder exempt statistics.
     */
    void appendExemptStats(BSONObjBuilder& b) const;

    /**
     * Bumps the delinquency counters associated with this queue. This intended to be called when
     * an operation completes, with the value of each of the delinquency counters accumulated
     * during its execution.
     */
    void incrementDelinquencyStats(const admission::execution_control::DelinquencyStats& newStats);

private:
    /**
     * Statistics for queueing mechanisms in the TicketHolder implementations. The term "Queue" is a
     * loose abstraction for the way in which operations are queued when there are no available
     * tickets.
     */
    struct QueueStats {
        AtomicWord<std::int64_t> totalAddedQueue{0};
        AtomicWord<std::int64_t> totalRemovedQueue{0};
        AtomicWord<std::int64_t> totalFinishedProcessing{0};
        AtomicWord<std::int64_t> totalNewAdmissions{0};
        AtomicWord<std::int64_t> totalTimeProcessingMicros{0};
        AtomicWord<std::int64_t> totalStartedProcessing{0};
        AtomicWord<std::int64_t> totalCanceled{0};
        AtomicWord<std::int64_t> totalTimeQueuedMicros{0};
    };

    /**
     * Generates and returns a ticket to the caller.
     */
    Ticket _issueTicket(AdmissionContext* admCtx);

    /**
     * Releases a ticket back into the ticket pool and updates queueing statistics. Tickets
     * issued for exempt operations do not get deposited back to the pool.
     * This function must not throw.
     */
    void _releaseTicket(Ticket& ticket);

    boost::optional<Ticket> _waitForTicketUntilMaybeInterruptible(OperationContext* opCtx,
                                                                  AdmissionContext* admCtx,
                                                                  Date_t until,
                                                                  bool interruptible);

    void _updatePeakUsed();

    const bool _trackPeakUsed;

    /**
     * Updates statistics and notifies the observer for a ticket acquisition at the given priority.
     */
    void _recordAcquisition(AdmissionContext* admCtx, AdmissionContext::Priority priority);

    /**
     * Appends the statistics stored in QueueStats to BSONObjBuilder b; We track statistics
     * for normalPriority operations and operations that are exempt from queueing.
     */
    void _appendQueueStats(BSONObjBuilder& b, const QueueStats& stats) const;

    void _immediateResize(WithLock, int newSize);

    QueueStats _holderStats;
    QueueStats _exemptStats;
    ResizePolicy _resizePolicy;
    TickSource* _tickSource;

    // Serializes updates to _outof to ensure only 1 thread can change the size of the ticket pool
    // at a time. Reading _outof does not require holding the lock.
    stdx::mutex _resizeMutex;
    Atomic<int> _outof;
    Atomic<int> _peakUsed;
    bool _enabledDelinquent{false};
    Milliseconds _delinquentMs{0};
    DelinquentCallback _reportDelinquentOpCallback{nullptr};
    AcquisitionCallback _reportAcquisitionOpCallback{nullptr};
    WaitedAcquisitionCallback _reportWaitedAcquisitionOpCallback{nullptr};
    ReleaseCallback _reportReleaseOpCallback{nullptr};
    mongo::admission::execution_control::DelinquencyStats _delinquencyStats;

    // Synchronization mechanism for waiters.
    std::unique_ptr<TicketSemaphore> _semaphore;
};

/**
 * RAII-style movable token that gets generated when a ticket is acquired and is automatically
 * released when going out of scope.
 */
class MONGO_MOD_PUBLIC Ticket {
    Ticket(const Ticket&) = delete;
    Ticket& operator=(const Ticket&) = delete;

    friend class TicketHolder;

public:
    Ticket(Ticket&& t)
        : _ticketholder(t._ticketholder),
          _admissionContext(t._admissionContext),
          _priority(t._priority),
          _acquisitionTime(t._acquisitionTime) {
        t._ticketholder = nullptr;
        t._admissionContext = nullptr;
    }

    Ticket& operator=(Ticket&& t) {
        if (&t == this) {
            return *this;
        }

        invariant(!valid(), "Attempting to overwrite a valid ticket with another one");
        _ticketholder = std::exchange(t._ticketholder, nullptr);
        _admissionContext = std::exchange(t._admissionContext, nullptr);
        _priority = t._priority;
        _acquisitionTime = t._acquisitionTime;

        return *this;
    };

    ~Ticket() {
        if (_ticketholder) {
            _ticketholder->_releaseTicket(*this);
        }
    }

    /**
     * Returns whether or not a ticket is being held.
     */
    bool valid() {
        return _ticketholder != nullptr;
    }

    /**
     * Returns the ticket priority.
     */
    AdmissionContext::Priority getPriority() const {
        return _priority;
    }

    AdmissionContext* getAdmissionContext() const {
        return _admissionContext;
    }

private:
    Ticket(TicketHolder* ticketHolder, AdmissionContext* admissionContext)
        : _ticketholder(ticketHolder), _admissionContext(admissionContext) {
        _priority = admissionContext->getPriority();
        _acquisitionTime = ticketHolder->_tickSource->getTicks();
    }

    /**
     * Discards the ticket without releasing it back to the ticketholder.
     */
    void discard() {
        _admissionContext->markTicketReleased();
        _ticketholder = nullptr;
        _admissionContext = nullptr;
    }

    TicketHolder* _ticketholder;

    // NOTE: Always use the snapshotted priority stored in the Ticket, rather than reading the live
    // priority from its associated AdmissionContext. The priority of the AdmissionContext cannot be
    // assumed to match the one originally captured by this Ticket, as the RAII object that modified
    // it may no longer be alive. Using the live priority could therefore lead to incorrect metrics.
    AdmissionContext* _admissionContext;
    AdmissionContext::Priority _priority;

    TickSource::Tick _acquisitionTime;
};

}  // namespace mongo
