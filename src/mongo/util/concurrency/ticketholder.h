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
    virtual void resize(int newSize) noexcept {};

    /**
     * Immediately returns a ticket without impacting the number of tickets available. Reserved for
     * operations that should never be throttled by the ticketing mechanism.
     */
    virtual Ticket acquireImmediateTicket(AdmissionContext* admCtx) = 0;

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

private:
    /**
     * Restricted for releasing tickets acquired via "acquireImmediateTicket". Handles the release
     * of an immediate ticket, which should never be reused or returned to the ticketing pool of
     * available tickets.
     */
    virtual void _releaseImmediateTicket(AdmissionContext* admCtx) noexcept = 0;

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

    Ticket acquireImmediateTicket(AdmissionContext* admCtx) override final;

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
    void resize(int newSize) noexcept override;

    virtual int used() const {
        return outof() - available();
    }

    /**
     * The total number of tickets allotted to the ticket pool.
     */
    int outof() const {
        return _outof.loadRelaxed();
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

    /**
     * 'Immediate' tickets are acquired and released independent of the ticketing pool and queueing
     * system. Subclasses must define whether they wish to record statistics surrounding 'immediate'
     * tickets in addition to standard queueing statistics.
     *
     * Returns true if statistics surrounding 'immediate' tickets are to be tracked. False
     * otherwise.
     */
    virtual bool recordImmediateTicketStatistics() = 0;

private:
    void _releaseImmediateTicket(AdmissionContext* admCtx) noexcept final;

    void _releaseToTicketPool(AdmissionContext* admCtx) noexcept override final;

    virtual boost::optional<Ticket> _tryAcquireImpl(AdmissionContext* admCtx) = 0;

    virtual boost::optional<Ticket> _waitForTicketUntilImpl(OperationContext* opCtx,
                                                            AdmissionContext* admCtx,
                                                            Date_t until,
                                                            TicketHolder::WaitMode waitMode) = 0;

    virtual void _appendImplStats(BSONObjBuilder& b) const = 0;

    virtual void _releaseToTicketPoolImpl(AdmissionContext* admCtx) noexcept = 0;

    virtual void _resize(int newSize, int oldSize) noexcept = 0;

    /**
     * Fetches the queueing statistics corresponding to the 'admCtx'. All statistics that are queue
     * specific should be updated through the resulting 'QueueStats'.
     */
    virtual QueueStats& _getQueueStatsToUse(const AdmissionContext* admCtx) noexcept = 0;

    Mutex _resizeMutex = MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(2),
                                          "TicketHolderWithQueueingStats::_resizeMutex");
    AtomicWord<int> _outof;

protected:
    ServiceContext* _serviceContext;
};

class SemaphoreTicketHolder final : public TicketHolderWithQueueingStats {
public:
    explicit SemaphoreTicketHolder(int numTickets, ServiceContext* serviceContext);
    ~SemaphoreTicketHolder() override final;

    int available() const override final;

    int queued() const override final {
        auto removed = _semaphoreStats.totalRemovedQueue.loadRelaxed();
        auto added = _semaphoreStats.totalAddedQueue.loadRelaxed();
        return std::max(static_cast<int>(added - removed), 0);
    };

    bool recordImmediateTicketStatistics() noexcept override final {
        // Historically, operations that now acquire 'immediate' tickets bypassed the ticketing
        // mechanism completely. Preserve legacy behavior where 'immediate' ticketing is not tracked
        // in the statistics.
        return false;
    }

private:
    boost::optional<Ticket> _waitForTicketUntilImpl(OperationContext* opCtx,
                                                    AdmissionContext* admCtx,
                                                    Date_t until,
                                                    WaitMode waitMode) override final;

    boost::optional<Ticket> _tryAcquireImpl(AdmissionContext* admCtx) override final;
    void _releaseToTicketPoolImpl(AdmissionContext* admCtx) noexcept override final;

    void _appendImplStats(BSONObjBuilder& b) const override final;

    void _resize(int newSize, int oldSize) noexcept override final;

    QueueStats& _getQueueStatsToUse(const AdmissionContext* admCtx) noexcept override final {
        return _semaphoreStats;
    }
#if defined(__linux__)
    mutable sem_t _sem;

#else
    bool _tryAcquire();

    int _numTickets;
    Mutex _mutex =
        MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(0), "SemaphoreTicketHolder::_mutex");
    stdx::condition_variable _newTicket;
#endif
    QueueStats _semaphoreStats;
};

/**
 * A ticketholder implementation that centralises all ticket acquisition/releases.
 * Waiters will get placed in a specific internal queue according to some logic.
 * Releasers will wake up a waiter from a group chosen according to some logic.
 */
class PriorityTicketHolder : public TicketHolderWithQueueingStats {
protected:
public:
    explicit PriorityTicketHolder(int numTickets, ServiceContext* serviceContext);
    ~PriorityTicketHolder() override;

    int available() const override final;

    int queued() const override final;

    bool recordImmediateTicketStatistics() noexcept override final {
        return true;
    };

private:
    // Using a shared_mutex is fine here because usual considerations for avoiding them do not apply
    // in this case:
    //   * Operations are short and do not block while holding the lock (i.e. they only do CPU-bound
    //   work)
    //   * Writer starvation is not possible as there are a finite number of operations to be
    //   performed in the reader case. Once all tickets get released no other thread can take the
    //   shared lock.
    //
    // The alternative of using ResourceMutex is not appropriate as the class serves as a
    // concurrency primitive and is performance sensitive.
    using QueueMutex = std::shared_mutex;                    // NOLINT
    using ReleaserLockGuard = std::shared_lock<QueueMutex>;  // NOLINT
    using EnqueuerLockGuard = std::unique_lock<QueueMutex>;  // NOLINT

    enum class QueueType : unsigned int {
        LowPriorityQueue = 0,
        NormalPriorityQueue = 1,
        // Exclusively used for statistics tracking. This queue should never have any processes
        // 'queued'.
        ImmediatePriorityNoOpQueue = 2,
        QueueTypeSize = 3
    };
    class Queue {
    public:
        Queue(PriorityTicketHolder* holder, QueueType queueType)
            : _holder(holder), _queueType(queueType){};

        bool attemptToDequeue(const ReleaserLockGuard& releaserLock);

        bool enqueue(OperationContext* interruptible,
                     EnqueuerLockGuard& queueLock,
                     const Date_t& until,
                     WaitMode waitMode);

        int queuedElems() const {
            return _queuedThreads;
        }

        /**
         * Returns a reference to the Queue statistics that allows callers to update the statistics.
         */
        QueueStats& getStatsToUse() {
            return _stats;
        }
        /**
         * Returns a read-only reference to the Queue statistics.
         */
        const QueueStats& getStats() const {
            return _stats;
        }

        int getThreadsPendingToWake() const {
            return _threadsToBeWoken.load();
        }

    private:
        void _signalThreadWoken(const EnqueuerLockGuard& enqueuerLock);

        int _queuedThreads{0};
        AtomicWord<int> _threadsToBeWoken{0};
        stdx::condition_variable _cv;
        PriorityTicketHolder* _holder;
        QueueStats _stats;
        const QueueType _queueType;
    };


    boost::optional<Ticket> _tryAcquireImpl(AdmissionContext* admCtx) override final;

    boost::optional<Ticket> _waitForTicketUntilImpl(OperationContext* opCtx,
                                                    AdmissionContext* admCtx,
                                                    Date_t until,
                                                    WaitMode waitMode) override final;

    void _releaseToTicketPoolImpl(AdmissionContext* admCtx) noexcept override final;

    void _resize(int newSize, int oldSize) noexcept override final;

    QueueStats& _getQueueStatsToUse(const AdmissionContext* admCtx) noexcept override final;

    void _appendImplStats(BSONObjBuilder& b) const override final;

    bool _tryAcquireTicket();

    /**
     * Wakes up a waiting thread (if it exists) in order for it to attempt to obtain a ticket.
     * Implementors MUST wake at least one waiting thread if at least one thread is pending to be
     * woken between all the queues. In other words, attemptToDequeue on each non-empty Queue must
     * be called until either it returns true at least once or has been called on all queues.
     *
     * Care must be taken to ensure that only CPU-bound work is performed here and it doesn't block.
     *
     * When called the following invariants will be held:
     * - The number of items in each queue will not change during the execution
     * - No other thread will proceed to wait during the execution of the method
     */
    void _dequeueWaitingThread(const ReleaserLockGuard& releaserLock);

    /**
     * Returns whether there are higher priority threads pending to get a ticket in front of the
     * given queue type and not enough tickets for all of them.
     */
    bool _hasToWaitForHigherPriority(const EnqueuerLockGuard& lk, QueueType queue);

    /**
     * Selects the queue to use for the current thread given the provided arguments.
     */
    Queue& _getQueueToUse(const AdmissionContext* admCtx);

    std::array<Queue, static_cast<unsigned int>(QueueType::QueueTypeSize)> _queues;

    QueueMutex _queueMutex;
    AtomicWord<int> _ticketsAvailable;
    AtomicWord<int> _enqueuedElements;
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
            if (_admissionContext->getPriority() == AdmissionContext::Priority::kImmediate) {
                _ticketholder->_releaseImmediateTicket(_admissionContext);
            } else {
                _ticketholder->_releaseToTicketPool(_admissionContext);
            }
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
