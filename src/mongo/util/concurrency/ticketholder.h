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
#include "mongo/platform/mutex.h"
#include "mongo/util/assert_util_core.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/time_support.h"

namespace mongo {

class Ticket;
class PriorityTicketHolder;
class SemaphoreTicketHolder;

/**
 * Maintains and distributes tickets across operations from a limited pool of tickets. The ticketing
 * mechanism is required for global lock acquisition to reduce contention on storage engine
 * resources.
 */
class TicketHolder {
    friend class Ticket;

public:
    TicketHolder(ServiceContext* svcCtx, int32_t numTickets, bool trackPeakUsed);

    virtual ~TicketHolder() {}

    /**
     * Adjusts the total number of tickets allocated for the ticket pool to 'newSize'.
     *
     * Returns 'true' if the resize completed without reaching the 'deadline', and 'false'
     * otherwise.
     */
    bool resize(int32_t newSize, Date_t deadline = Date_t::max()) noexcept;

    /**
     * Attempts to acquire a ticket without blocking.
     * Returns a ticket if one is available, and boost::none otherwise.
     */
    virtual boost::optional<Ticket> tryAcquire(AdmissionContext* admCtx);

    /**
     * Attempts to acquire a ticket. Blocks until a ticket is acquired or the Interruptible is
     * interrupted, throwing an AssertionException. Outputs 'timeQueuedForTicketMicros' with time
     * spent waiting for a ticket.
     */
    virtual Ticket waitForTicket(Interruptible& interruptible,
                                 AdmissionContext* admCtx,
                                 Microseconds& timeQueuedForTicketMicros);

    /**
     * Attempts to acquire a ticket within a deadline, 'until'. Returns 'true' if a ticket is
     * acquired and 'false' if the deadline is reached, but the operation is retryable. Throws an
     * AssertionException if the Interruptible is interrupted. Outputs 'timeQueuedForTicketMicros'
     * with time spent waiting for a ticket.
     */
    virtual boost::optional<Ticket> waitForTicketUntil(Interruptible& interruptible,
                                                       AdmissionContext* admCtx,
                                                       Date_t until,
                                                       Microseconds& timeQueuedForTicketMicros);

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
     * Peak number of tickets checked out at once since the previous time this function was called.
     * Invariants that 'trackPeakUsed' has been passed to the TicketHolder,
     */
    int32_t getAndResetPeakUsed();

    virtual void appendStats(BSONObjBuilder& b) const;

    /**
     * Instantaneous number of tickets 'available' (not checked out by an operation) in the ticket
     * pool.
     */
    virtual int32_t available() const = 0;

    /**
     * Instantaneous number of operations waiting in queue for a ticket.
     *
     * TODO SERVER-74082: Once the SemaphoreTicketHolder is removed, consider changing this metric
     * to int32_t.
     */
    virtual int64_t queued() const = 0;

    /**
     * The total number of operations that acquired a ticket, completed their work, and released the
     * ticket.
     */
    virtual int64_t numFinishedProcessing() const = 0;

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

private:
    /**
     * Releases a ticket back into the ticketing pool.
     */
    virtual void _releaseToTicketPool(AdmissionContext* admCtx,
                                      AdmissionContext::Priority ticketPriority) noexcept;

    virtual void _releaseToTicketPoolImpl(AdmissionContext* admCtx) noexcept = 0;

    virtual boost::optional<Ticket> _tryAcquireImpl(AdmissionContext* admCtx) = 0;

    virtual boost::optional<Ticket> _waitForTicketUntilImpl(Interruptible& interruptible,
                                                            AdmissionContext* admCtx,
                                                            Date_t until) = 0;

    virtual void _appendImplStats(BSONObjBuilder& b) const {}

    /**
     * Fetches the queueing statistics for the given priority. All statistics that are queue
     * specific should be updated through the resulting 'QueueStats'.
     */
    virtual QueueStats& _getQueueStatsToUse(AdmissionContext::Priority priority) noexcept = 0;

    void _updatePeakUsed();

    const bool _trackPeakUsed;

    Mutex _resizeMutex =
        MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(2), "TicketHolder::_resizeMutex");
    QueueStats _exemptQueueStats;

protected:
    /**
     * Appends the standard statistics stored in QueueStats to BSONObjBuilder b;
     */
    void _appendCommonQueueImplStats(BSONObjBuilder& b, const QueueStats& stats) const;

    AtomicWord<int32_t> _outof;
    AtomicWord<int32_t> _peakUsed;

    ServiceContext* _serviceContext;
};

class MockTicketHolder : public TicketHolder {
public:
    MockTicketHolder(ServiceContext* svcCtx) : TicketHolder(svcCtx, 0, true) {}

    boost::optional<Ticket> tryAcquire(AdmissionContext* admCtx) override;

    Ticket waitForTicket(Interruptible& interruptible,
                         AdmissionContext* admCtx,
                         Microseconds& timeQueuedForTicketMicros) override;

    boost::optional<Ticket> waitForTicketUntil(Interruptible& interruptible,
                                               AdmissionContext* admCtx,
                                               Date_t until,
                                               Microseconds& timeQueuedForTicketMicros) override;

    void appendStats(BSONObjBuilder& b) const override {}

    int32_t available() const override {
        return _available;
    }

    void setPeakUsed(int32_t used) {
        _peakUsed.store(used);
    }

    void setAvailable(int32_t available) {
        _available = available;
    }

    int64_t queued() const override {
        return 0;
    }

    int64_t numFinishedProcessing() const override {
        return _numFinishedProcessing;
    }

    void setNumFinishedProcessing(int32_t numFinishedProcessing) {
        _numFinishedProcessing = numFinishedProcessing;
    }

private:
    void _releaseToTicketPoolImpl(AdmissionContext* admCtx) noexcept override;

    boost::optional<Ticket> _tryAcquireImpl(AdmissionContext* admCtx) override;

    boost::optional<Ticket> _waitForTicketUntilImpl(Interruptible& interruptible,
                                                    AdmissionContext* admCtx,
                                                    Date_t until) override;

    QueueStats& _getQueueStatsToUse(AdmissionContext::Priority priority) noexcept override {
        return _stats;
    }

    QueueStats _stats;

    int32_t _available = 0;
    int32_t _numFinishedProcessing = 0;
};

/**
 * RAII-style movable token that gets generated when a ticket is acquired and is automatically
 * released when going out of scope.
 */
class Ticket {
    friend class TicketHolder;
    friend class SemaphoreTicketHolder;
    friend class PriorityTicketHolder;
    friend class MockTicketHolder;

public:
    Ticket(Ticket&& t)
        : _ticketholder(t._ticketholder),
          _admissionContext(t._admissionContext),
          _priority(t._priority) {
        t._ticketholder = nullptr;
        t._admissionContext = nullptr;
    }

    Ticket& operator=(Ticket&& t) {
        if (&t == this) {
            return *this;
        }
        invariant(!valid(), "Attempting to overwrite a valid ticket with another one");
        _ticketholder = t._ticketholder;
        _admissionContext = t._admissionContext;
        _priority = t._priority;
        t._ticketholder = nullptr;
        t._admissionContext = nullptr;
        return *this;
    };

    ~Ticket() {
        if (_ticketholder) {
            _ticketholder->_releaseToTicketPool(_admissionContext, _priority);
        }
    }

    // TODO(SERVER-86112): Remove
    static Ticket ephemeral(AdmissionContext& admissionContext) {
        return Ticket(nullptr, &admissionContext);
    }

    /**
     * Returns whether or not a ticket is being held.
     */
    bool valid() {
        return _ticketholder != nullptr;
    }

private:
    Ticket(TicketHolder* ticketHolder, AdmissionContext* admissionContext)
        : _ticketholder(ticketHolder), _admissionContext(admissionContext) {
        _priority = admissionContext->getPriority();
    }

    /**
     * Discards the ticket without releasing it back to the ticketholder.
     */
    void discard() {
        _ticketholder = nullptr;
        _admissionContext = nullptr;
    }

    // No copy constructors.
    Ticket(const Ticket&) = delete;
    Ticket& operator=(const Ticket&) = delete;

    TicketHolder* _ticketholder;
    AdmissionContext* _admissionContext;
    AdmissionContext::Priority _priority;
};
}  // namespace mongo
