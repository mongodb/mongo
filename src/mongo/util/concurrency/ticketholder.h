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

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <cstdint>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/waitable_atomic.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util_core.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/tick_source.h"
#include "mongo/util/time_support.h"
#include <boost/optional/optional.hpp>

namespace mongo {

class Ticket;

/**
 * Maintains and distributes tickets across operations from a limited pool of tickets. The ticketing
 * mechanism is required for global lock acquisition to reduce contention on storage engine
 * resources.
 */
class TicketHolder {
    friend class Ticket;

public:
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

    TicketHolder(ServiceContext* serviceContext,
                 int numTickets,
                 bool trackPeakUsed,
                 ResizePolicy resizePolicy = ResizePolicy::kGradual);

    /**
     * Adjusts the total number of tickets allocated for the ticket pool to 'newSize'.
     *
     * Returns 'true' if the resize completed without reaching the 'deadline', and 'false'
     * otherwise.
     */
    bool resize(OperationContext* opCtx, int32_t newSize, Date_t deadline = Date_t::max());

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
    int32_t outof() const {
        return _outof.loadRelaxed();
    }

    /**
     * Instantaneous number of tickets that are checked out by an operation.
     */
    int32_t used() const {
        return outof() - available();
    }

    /**
     * Instantaneous number of operations waiting in queue for a ticket.
     * TODO SERVER-74082: Consider changing this metric to int32_t.
     */
    int64_t queued() const;

    /**
     * Peak number of tickets checked out at once since the previous time this function was called.
     * Invariants that 'trackPeakUsed' has been passed to the TicketHolder,
     */
    int32_t getAndResetPeakUsed();

    /**
     * Instantaneous number of tickets 'available' (not checked out by an operation) in the ticket
     * pool.
     */
    int32_t available() const;

    /**
     * The total number of operations that acquired a ticket, completed their work, and released the
     * ticket.
     */
    int64_t numFinishedProcessing() const;

    void setNumFinishedProcessing_forTest(int32_t numFinishedProcessing);

    void setPeakUsed_forTest(int32_t used);

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
     * Append TicketHolder statistics to the provided builder.
     */
    void appendStats(BSONObjBuilder& b) const;

private:
    /**
     * Releases a ticket back into the ticket pool and updates queueing statistics. Tickets
     * issued for exempt operations do not get deposited back to the pool.
     */
    void _releaseTicketUpdateStats(Ticket& ticket) noexcept;

    void _releaseNormalPriorityTicket(AdmissionContext* admCtx) noexcept;

    boost::optional<Ticket> _tryAcquireNormalPriorityTicket(AdmissionContext* admCtx);

    boost::optional<Ticket> _waitForTicketUntilMaybeInterruptible(OperationContext* opCtx,
                                                                  AdmissionContext* admCtx,
                                                                  Date_t until,
                                                                  bool interruptible);
    boost::optional<Ticket> _performWaitForTicketUntil(OperationContext* opCtx,
                                                       AdmissionContext* admCtx,
                                                       Date_t until,
                                                       bool interruptible);

    void _updatePeakUsed();

    const bool _trackPeakUsed;

    void _updateQueueStatsOnRelease(TicketHolder::QueueStats& queueStats, const Ticket& ticket);
    void _updateQueueStatsOnTicketAcquisition(AdmissionContext* admCtx,
                                              TicketHolder::QueueStats& queueStats);

    /**
     * Appends the statistics stored in QueueStats to BSONObjBuilder b; We track statistics
     * for normalPriority operations and operations that are exempt from queueing.
     */
    void _appendQueueStats(BSONObjBuilder& b, const QueueStats& stats) const;

    void _immediateResize(WithLock, int32_t newSize);

    /**
     * Creates a ticket for a non-exempt admission.
     */
    Ticket _makeTicket(AdmissionContext* admCtx);

    QueueStats _normalPriorityQueueStats;
    QueueStats _exemptQueueStats;
    ResizePolicy _resizePolicy;
    ServiceContext* _serviceContext;

    // Serializes updates to _outof to ensure only 1 thread can change the size of the ticket pool
    // at a time. Reading _outof does not require holding the lock.
    stdx::mutex _resizeMutex;
    BasicWaitableAtomic<int32_t> _tickets;
    Atomic<int32_t> _waiterCount{0};
    AtomicWord<int32_t> _outof;
    AtomicWord<int32_t> _peakUsed;
};

/**
 * RAII-style movable token that gets generated when a ticket is acquired and is automatically
 * released when going out of scope.
 */
class Ticket {
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
            _ticketholder->_releaseTicketUpdateStats(*this);
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

private:
    Ticket(TicketHolder* ticketHolder, AdmissionContext* admissionContext)
        : _ticketholder(ticketHolder), _admissionContext(admissionContext) {
        _priority = admissionContext->getPriority();
        _acquisitionTime = ticketHolder->_serviceContext->getTickSource()->getTicks();
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
    AdmissionContext* _admissionContext;
    AdmissionContext::Priority _priority;
    TickSource::Tick _acquisitionTime;
};

inline Ticket TicketHolder::_makeTicket(AdmissionContext* admCtx) {
    // TODO(SERVER-92647): Move this to the Ticket constructor so it also applies to exempt tickets
    admCtx->markTicketHeld();
    return Ticket{this, admCtx};
}

}  // namespace mongo
