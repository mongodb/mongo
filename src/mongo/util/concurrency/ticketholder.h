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

#if defined(__linux__)
#include <semaphore.h>
#endif

#include <queue>

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

class TicketHolder {
    friend class Ticket;

public:
    /**
     * Wait mode for ticket acquisition: interruptible or uninterruptible.
     */
    enum WaitMode { kInterruptible, kUninterruptible };

    TicketHolder(int num, ServiceContext* svcCtx) : _outof(num), _serviceContext(svcCtx){};

    virtual ~TicketHolder() = 0;

    /**
     * Attempts to acquire a ticket without blocking.
     * Returns a boolean indicating whether the operation was successful or not.
     */
    boost::optional<Ticket> tryAcquire(AdmissionContext* admCtx);

    /**
     * Attempts to acquire a ticket. Blocks until a ticket is acquired or the OperationContext
     * 'opCtx' is killed, throwing an AssertionException.
     */
    Ticket waitForTicket(OperationContext* opCtx, AdmissionContext* admCtx, WaitMode waitMode);

    /**
     * Attempts to acquire a ticket within a deadline, 'until'. Returns 'true' if a ticket is
     * acquired and 'false' if the deadline is reached, but the operation is retryable. Throws an
     * AssertionException if the OperationContext 'opCtx' is killed and no waits for tickets can
     * proceed.
     */
    boost::optional<Ticket> waitForTicketUntil(OperationContext* opCtx,
                                               AdmissionContext* admCtx,
                                               Date_t until,
                                               WaitMode waitMode);

    Status resize(int newSize);

    virtual int available() const = 0;

    virtual int used() const {
        return outof() - available();
    }

    virtual int outof() const {
        return _outof.loadRelaxed();
    }

    virtual int queued() const {
        auto removed = _totalRemovedQueue.loadRelaxed();
        auto added = _totalAddedQueue.loadRelaxed();
        return std::max(static_cast<int>(added - removed), 0);
    }

    void appendStats(BSONObjBuilder& b) const;

private:
    virtual boost::optional<Ticket> _tryAcquireImpl(AdmissionContext* admCtx) = 0;

    virtual boost::optional<Ticket> _waitForTicketUntilImpl(OperationContext* opCtx,
                                                            AdmissionContext* admCtx,
                                                            Date_t until,
                                                            WaitMode waitMode) = 0;

    virtual void _appendImplStats(BSONObjBuilder& b) const = 0;

    void _releaseAndUpdateMetrics(AdmissionContext* admCtx) noexcept;

    virtual void _release(AdmissionContext* admCtx) noexcept = 0;

    AtomicWord<std::int64_t> _totalAddedQueue{0};
    AtomicWord<std::int64_t> _totalRemovedQueue{0};
    AtomicWord<std::int64_t> _totalFinishedProcessing{0};
    AtomicWord<std::int64_t> _totalNewAdmissions{0};
    AtomicWord<std::int64_t> _totalTimeProcessingMicros{0};
    AtomicWord<std::int64_t> _totalStartedProcessing{0};
    AtomicWord<std::int64_t> _totalCanceled{0};

    Mutex _resizeMutex =
        MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(2), "TicketHolder::_resizeMutex");
    AtomicWord<int> _outof;

protected:
    ServiceContext* _serviceContext;
};

class SemaphoreTicketHolder final : public TicketHolder {
public:
    explicit SemaphoreTicketHolder(int num, ServiceContext* serviceContext);
    ~SemaphoreTicketHolder() override final;

    int available() const override final;

private:
    boost::optional<Ticket> _waitForTicketUntilImpl(OperationContext* opCtx,
                                                    AdmissionContext* admCtx,
                                                    Date_t until,
                                                    WaitMode waitMode) override final;

    boost::optional<Ticket> _tryAcquireImpl(AdmissionContext* admCtx) override final;
    void _release(AdmissionContext* admCtx) noexcept override final;

    void _appendImplStats(BSONObjBuilder& b) const override final;

#if defined(__linux__)
    mutable sem_t _sem;

#else
    bool _tryAcquire();

    int _num;
    Mutex _mutex =
        MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(0), "SemaphoreTicketHolder::_mutex");
    stdx::condition_variable _newTicket;
#endif

    // Implementation statistics.
    AtomicWord<std::int64_t> _totalTimeQueuedMicros{0};
};

/**
 * A ticketholder implementation that uses a queue for pending operations.
 * Any change to the implementation should be paired with a change to the _ticketholder.tla_ file in
 * order to formally verify that the changes won't lead to a deadlock.
 */
class FifoTicketHolder final : public TicketHolder {
public:
    explicit FifoTicketHolder(int num, ServiceContext* serviceContext);
    ~FifoTicketHolder() override final;

    int available() const override final;

    int queued() const override final {
        return _enqueuedElements.load();
    }

private:
    boost::optional<Ticket> _waitForTicketUntilImpl(OperationContext* opCtx,
                                                    AdmissionContext* admCtx,
                                                    Date_t until,
                                                    WaitMode waitMode) override final;

    boost::optional<Ticket> _tryAcquireImpl(AdmissionContext* admCtx) override final;

    void _appendImplStats(BSONObjBuilder& b) const override final;

    void _release(AdmissionContext* admCtx) noexcept override final;

    // Implementation statistics.
    AtomicWord<std::int64_t> _totalTimeQueuedMicros{0};

    enum class WaitingState { Waiting, Cancelled, Assigned };
    struct WaitingElement {
        stdx::condition_variable signaler;
        Mutex modificationMutex = MONGO_MAKE_LATCH(
            HierarchicalAcquisitionLevel(0), "FifoTicketHolder::WaitingElement::modificationMutex");
        WaitingState state;
    };
    std::queue<std::shared_ptr<WaitingElement>> _queue;
    // _queueMutex protects all modifications made to either the _queue, or the statistics of the
    // queue.
    Mutex _queueMutex =
        MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(1), "FifoTicketHolder::_queueMutex");
    AtomicWord<int> _enqueuedElements;
    AtomicWord<int> _ticketsAvailable;
};

/**
 * RAII-style movable token that gets generated when a ticket is acquired and is automatically
 * released when going out of scope.
 */
class Ticket {
    friend class TicketHolder;
    friend class SemaphoreTicketHolder;
    friend class FifoTicketHolder;

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
            _ticketholder->_releaseAndUpdateMetrics(_admissionContext);
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
