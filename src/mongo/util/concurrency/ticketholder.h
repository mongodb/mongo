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

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/future.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/concurrency/mutex.h"
#include "mongo/util/hierarchical_acquisition.h"
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
    virtual ~TicketHolder(){};

    /**
     * Wait mode for ticket acquisition: interruptible or uninterruptible.
     */
    enum WaitMode { kInterruptible, kUninterruptible };

    /**
     * Adjusts the total number of tickets allocated for the ticket pool to 'newSize'.
     */
    virtual void resize(OperationContext* opCtx, int newSize) noexcept {};

    /**
     * Attempts to acquire a ticket without blocking.
     * Returns a boolean indicating whether the operation was successful or not.
     */
    virtual boost::optional<Ticket> tryAcquire(AdmissionContext* admCtx) = 0;

    /**
     * Attempts to acquire a ticket. Blocks until a ticket is acquired or the OperationContext
     * 'opCtx' is killed, throwing an AssertionException.
     */
    virtual Ticket waitForTicket(OperationContext* opCtx,
                                 AdmissionContext* admCtx,
                                 WaitMode waitMode) = 0;

    /**
     * Attempts to acquire a ticket within a deadline, 'until'. Returns 'true' if a ticket is
     * acquired and 'false' if the deadline is reached, but the operation is retryable. Throws an
     * AssertionException if the OperationContext 'opCtx' is killed and no waits for tickets can
     * proceed.
     */
    virtual boost::optional<Ticket> waitForTicketUntil(OperationContext* opCtx,
                                                       AdmissionContext* admCtx,
                                                       Date_t until,
                                                       WaitMode waitMode) = 0;

    virtual void appendStats(BSONObjBuilder& b) const = 0;

    /**
     * 'Immediate' admissions don't need to acquire or wait for a ticket. However, they should
     * report to the TicketHolder for tracking purposes.
     *
     * Increments the count of 'immediate' priority admissions reported.
     */
    virtual void reportImmediatePriorityAdmission() = 0;

private:
    /**
     * Releases a ticket back into the ticketing pool.
     */
    virtual void _releaseToTicketPool(AdmissionContext* admCtx) noexcept = 0;
};

/**
 * A ticketholder which manages both aggregate and policy specific queueing statistics.
 */
class TicketHolderWithQueueingStats : public TicketHolder {
public:
    TicketHolderWithQueueingStats(int numTickets, ServiceContext* svcCtx)
        : _outof(numTickets), _serviceContext(svcCtx){};

    ~TicketHolderWithQueueingStats() override{};

    boost::optional<Ticket> tryAcquire(AdmissionContext* admCtx) override;

    Ticket waitForTicket(OperationContext* opCtx,
                         AdmissionContext* admCtx,
                         TicketHolder::WaitMode waitMode) override;

    boost::optional<Ticket> waitForTicketUntil(OperationContext* opCtx,
                                               AdmissionContext* admCtx,
                                               Date_t until,
                                               TicketHolder::WaitMode waitMode) override;

    /**
     * Adjusts the total number of tickets allocated for the ticket pool to 'newSize'.
     */
    void resize(OperationContext* opCtx, int newSize) noexcept override;

    virtual int used() const {
        return outof() - available();
    }

    /**
     * The total number of tickets allotted to the ticket pool.
     */
    int outof() const {
        return _outof.loadRelaxed();
    }

    /**
     * Returns the number of 'immediate' priority admissions, which always bypass ticket
     * acquisition.
     */
    int64_t getImmediatePriorityAdmissionsCount() const {
        return _immediatePriorityAdmissionsCount.loadRelaxed();
    }

    void reportImmediatePriorityAdmission() override final {
        _immediatePriorityAdmissionsCount.fetchAndAdd(1);
    }

    void appendStats(BSONObjBuilder& b) const override;

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
     * Instantaneous number of operations waiting in queue for a ticket.
     */
    virtual int queued() const = 0;

    /**
     * Instantaneous number of tickets 'available' (not checked out by an operation) in the ticket
     * pool.
     */
    virtual int available() const = 0;

private:
    void _releaseToTicketPool(AdmissionContext* admCtx) noexcept override final;

    virtual boost::optional<Ticket> _tryAcquireImpl(AdmissionContext* admCtx) = 0;

    virtual boost::optional<Ticket> _waitForTicketUntilImpl(OperationContext* opCtx,
                                                            AdmissionContext* admCtx,
                                                            Date_t until,
                                                            TicketHolder::WaitMode waitMode) = 0;

    virtual void _appendImplStats(BSONObjBuilder& b) const = 0;

    virtual void _releaseToTicketPoolImpl(AdmissionContext* admCtx) noexcept = 0;

    virtual void _resize(OperationContext* opCtx, int newSize, int oldSize) noexcept = 0;

    /**
     * Fetches the queueing statistics corresponding to the 'admCtx'. All statistics that are queue
     * specific should be updated through the resulting 'QueueStats'.
     */
    virtual QueueStats& _getQueueStatsToUse(const AdmissionContext* admCtx) noexcept = 0;

    Mutex _resizeMutex = MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(2),
                                          "TicketHolderWithQueueingStats::_resizeMutex");
    AtomicWord<int> _outof;
    AtomicWord<std::int64_t> _immediatePriorityAdmissionsCount;

protected:
    /**
     * Appends the standard statistics stored in QueueStats to BSONObjBuilder b;
     */
    void _appendCommonQueueImplStats(BSONObjBuilder& b, const QueueStats& stats) const;

    ServiceContext* _serviceContext;
};

/**
 * RAII-style movable token that gets generated when a ticket is acquired and is automatically
 * released when going out of scope.
 */
class Ticket {
    friend class TicketHolder;
    friend class TicketHolderWithQueueingStats;
    friend class SemaphoreTicketHolder;
    friend class PriorityTicketHolder;

public:
    Ticket(Ticket&& t) : _ticketholder(t._ticketholder), _admissionContext(t._admissionContext) {
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
        t._ticketholder = nullptr;
        t._admissionContext = nullptr;
        return *this;
    };

    ~Ticket() {
        if (_ticketholder) {
            _ticketholder->_releaseToTicketPool(_admissionContext);
        }
    }

    /**
     * Returns whether or not a ticket is being held.
     */
    bool valid() {
        return _ticketholder != nullptr;
    }

private:
    Ticket(TicketHolder* ticketHolder, AdmissionContext* admissionContext)
        : _ticketholder(ticketHolder), _admissionContext(admissionContext) {}

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
};
}  // namespace mongo
