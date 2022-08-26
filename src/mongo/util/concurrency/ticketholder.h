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
class ReaderWriterTicketHolder;

/**
 * A ticket mechanism is required for global lock acquisition to reduce contention on storage
 * engine resources.
 *
 * Manages the distribution of tickets across operations.
 */
class TicketHolder {
    friend class Ticket;

public:
    virtual ~TicketHolder(){};

    /**
     * Wait mode for ticket acquisition: interruptible or uninterruptible.
     */
    enum WaitMode { kInterruptible, kUninterruptible };

    static TicketHolder* get(ServiceContext* svcCtx);

    static void use(ServiceContext* svcCtx, std::unique_ptr<TicketHolder> newTicketHolder);

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
    virtual void _release(AdmissionContext* admCtx) noexcept = 0;
};

class TicketHolderWithQueueingStats : public TicketHolder {
    friend class ReaderWriterTicketHolder;

public:
    /**
     * Wait mode for ticket acquisition: interruptible or uninterruptible.
     */
    TicketHolderWithQueueingStats(int numTickets, ServiceContext* svcCtx)
        : _outof(numTickets), _serviceContext(svcCtx){};

    ~TicketHolderWithQueueingStats() override{};

    /**
     * Attempts to acquire a ticket without blocking.
     * Returns a boolean indicating whether the operation was successful or not.
     */
    boost::optional<Ticket> tryAcquire(AdmissionContext* admCtx) override;

    /**
     * Attempts to acquire a ticket. Blocks until a ticket is acquired or the OperationContext
     * 'opCtx' is killed, throwing an AssertionException.
     */
    Ticket waitForTicket(OperationContext* opCtx,
                         AdmissionContext* admCtx,
                         TicketHolder::WaitMode waitMode) override;

    /**
     * Attempts to acquire a ticket within a deadline, 'until'. Returns 'true' if a ticket is
     * acquired and 'false' if the deadline is reached, but the operation is retryable. Throws an
     * AssertionException if the OperationContext 'opCtx' is killed and no waits for tickets can
     * proceed.
     */
    boost::optional<Ticket> waitForTicketUntil(OperationContext* opCtx,
                                               AdmissionContext* admCtx,
                                               Date_t until,
                                               TicketHolder::WaitMode waitMode) override;

    Status resize(int newSize);

    virtual int available() const = 0;

    virtual int used() const {
        return outof() - available();
    }

    int outof() const {
        return _outof.loadRelaxed();
    }

    virtual int queued() const {
        auto removed = _totalRemovedQueue.loadRelaxed();
        auto added = _totalAddedQueue.loadRelaxed();
        return std::max(static_cast<int>(added - removed), 0);
    }

    void appendStats(BSONObjBuilder& b) const override;

private:
    virtual boost::optional<Ticket> _tryAcquireImpl(AdmissionContext* admCtx) = 0;

    virtual boost::optional<Ticket> _waitForTicketUntilImpl(OperationContext* opCtx,
                                                            AdmissionContext* admCtx,
                                                            Date_t until,
                                                            TicketHolder::WaitMode waitMode) = 0;

    virtual void _appendImplStats(BSONObjBuilder& b) const = 0;

    void _release(AdmissionContext* admCtx) noexcept override;

    virtual void _releaseQueue(AdmissionContext* admCtx) noexcept = 0;

    AtomicWord<std::int64_t> _totalAddedQueue{0};
    AtomicWord<std::int64_t> _totalRemovedQueue{0};
    AtomicWord<std::int64_t> _totalFinishedProcessing{0};
    AtomicWord<std::int64_t> _totalNewAdmissions{0};
    AtomicWord<std::int64_t> _totalTimeProcessingMicros{0};
    AtomicWord<std::int64_t> _totalStartedProcessing{0};
    AtomicWord<std::int64_t> _totalCanceled{0};

    Mutex _resizeMutex = MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(2),
                                          "TicketHolderWithQueueingStats::_resizeMutex");
    AtomicWord<int> _outof;

protected:
    ServiceContext* _serviceContext;
};

/**
 * A TicketHolder implementation that delegates actual ticket management to two underlying
 * TicketHolderQueues. The routing decision will be based on the lock mode requested by the caller
 * directing MODE_IS/MODE_S requests to the "Readers" TicketHolderWithQueueingStats and MODE_IX
 * requests to the "Writers" TicketHolderWithQueueingStats.
 */
class ReaderWriterTicketHolder final : public TicketHolder {
public:
    ReaderWriterTicketHolder(std::unique_ptr<TicketHolderWithQueueingStats> readerTicketHolder,
                             std::unique_ptr<TicketHolderWithQueueingStats> writerTicketHolder)
        : _reader(std::move(readerTicketHolder)), _writer(std::move(writerTicketHolder)){};

    ~ReaderWriterTicketHolder() override final;

    /**
     * Attempts to acquire a ticket without blocking.
     * Returns a boolean indicating whether the operation was successful or not.
     */
    boost::optional<Ticket> tryAcquire(AdmissionContext* admCtx) override final;

    /**
     * Attempts to acquire a ticket. Blocks until a ticket is acquired or the OperationContext
     * 'opCtx' is killed, throwing an AssertionException.
     */
    Ticket waitForTicket(OperationContext* opCtx,
                         AdmissionContext* admCtx,
                         WaitMode waitMode) override final;

    /**
     * Attempts to acquire a ticket within a deadline, 'until'. Returns 'true' if a ticket is
     * acquired and 'false' if the deadline is reached, but the operation is retryable. Throws an
     * AssertionException if the OperationContext 'opCtx' is killed and no waits for tickets can
     * proceed.
     */
    boost::optional<Ticket> waitForTicketUntil(OperationContext* opCtx,
                                               AdmissionContext* admCtx,
                                               Date_t until,
                                               WaitMode waitMode) override final;

    void appendStats(BSONObjBuilder& b) const override final;

    Status resizeReaders(int newSize);
    Status resizeWriters(int newSize);

private:
    void _release(AdmissionContext* admCtx) noexcept override final;

private:
    std::unique_ptr<TicketHolderWithQueueingStats> _reader;
    std::unique_ptr<TicketHolderWithQueueingStats> _writer;
};

class SemaphoreTicketHolder final : public TicketHolderWithQueueingStats {
public:
    explicit SemaphoreTicketHolder(int numTickets, ServiceContext* serviceContext);
    ~SemaphoreTicketHolder() override final;

    int available() const override final;

private:
    boost::optional<Ticket> _waitForTicketUntilImpl(OperationContext* opCtx,
                                                    AdmissionContext* admCtx,
                                                    Date_t until,
                                                    WaitMode waitMode) override final;

    boost::optional<Ticket> _tryAcquireImpl(AdmissionContext* admCtx) override final;
    void _releaseQueue(AdmissionContext* admCtx) noexcept override final;

    void _appendImplStats(BSONObjBuilder& b) const override final;

#if defined(__linux__)
    mutable sem_t _sem;

#else
    bool _tryAcquire();

    int _numTickets;
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
class FifoTicketHolder final : public TicketHolderWithQueueingStats {
public:
    explicit FifoTicketHolder(int numTickets, ServiceContext* serviceContext);
    ~FifoTicketHolder() override final;

    int available() const override final;

    int queued() const override final;

private:
    boost::optional<Ticket> _waitForTicketUntilImpl(OperationContext* opCtx,
                                                    AdmissionContext* admCtx,
                                                    Date_t until,
                                                    WaitMode waitMode) override final;

    boost::optional<Ticket> _tryAcquireImpl(AdmissionContext* admCtx) override final;

    void _appendImplStats(BSONObjBuilder& b) const override final;

    void _releaseQueue(AdmissionContext* admCtx) noexcept override final;

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
 * A ticketholder implementation that centralises all ticket acquisition/releases.
 * Waiters will get placed in a specific internal queue according to some logic.
 * Releasers will wake up a waiter from a group chosen according to some logic.
 */
class SchedulingTicketHolder : public TicketHolderWithQueueingStats {
    using QueueMutex = std::shared_mutex;                    // NOLINT
    using ReleaserLockGuard = std::shared_lock<QueueMutex>;  // NOLINT
    using EnqueuerLockGuard = std::unique_lock<QueueMutex>;  // NOLINT
protected:
    class Queue {
    public:
        Queue(SchedulingTicketHolder* holder) : _holder(holder){};

        Queue(Queue&& other)
            : _queuedThreads(other._queuedThreads),
              _threadsToBeWoken(other._threadsToBeWoken.load()),
              _holder(other._holder){};

        bool attemptToDequeue();

        bool enqueue(OperationContext* interruptible,
                     EnqueuerLockGuard& queueLock,
                     const Date_t& until,
                     WaitMode waitMode);

        int queuedElems() const {
            return _queuedThreads;
        }

    private:
        void _signalThreadWoken();

        int _queuedThreads{0};
        AtomicWord<int> _threadsToBeWoken{0};
        stdx::condition_variable _cv;
        SchedulingTicketHolder* _holder;
    };

    std::vector<Queue> _queues;

public:
    explicit SchedulingTicketHolder(int numTickets,
                                    unsigned int numQueues,
                                    ServiceContext* serviceContext);
    ~SchedulingTicketHolder() override;

    int available() const override final;

    int queued() const override final;

private:
    bool _tryAcquireTicket();

    boost::optional<Ticket> _tryAcquireImpl(AdmissionContext* admCtx) override final;

    boost::optional<Ticket> _waitForTicketUntilImpl(OperationContext* opCtx,
                                                    AdmissionContext* admCtx,
                                                    Date_t until,
                                                    WaitMode waitMode) override final;

    void _releaseQueue(AdmissionContext* admCtx) noexcept override final;

    void _appendImplStats(BSONObjBuilder& b) const override final{};

    /**
     * Wakes up a waiting thread (if it exists) in order for it to attempt to obtain a ticket.
     * Implementors MUST wake at least one waiting thread if at least one thread is pending to be
     * woken between all the queues. In other words, attemptToDequeue on each non-empty Queue must
     * be called until either it returns true at least once or has been called on all queues.
     *
     * When called the following invariants will be held:
     * - The number of items in each queue will not change during the execution
     * - No other thread will proceed to wait during the execution of the method
     */
    virtual void _dequeueWaitingThread() = 0;

    /**
     * Selects the queue to use for the current thread given the provided arguments.
     */
    virtual Queue& _getQueueToUse(OperationContext* opCtx, const AdmissionContext* admCtx) = 0;

    QueueMutex _queueMutex;
    AtomicWord<int> _ticketsAvailable;
    AtomicWord<int> _enqueuedElements;
    ServiceContext* _serviceContext;
};

class StochasticTicketHolder final : public SchedulingTicketHolder {
public:
    explicit StochasticTicketHolder(int numTickets,
                                    int readerWeight,
                                    int writerWeight,
                                    ServiceContext* serviceContext);

private:
    enum class QueueType : unsigned int { ReaderQueue = 0, WriterQueue = 1 };

    void _dequeueWaitingThread() override final;

    Queue& _getQueueToUse(OperationContext* opCtx, const AdmissionContext* admCtx) override final;

    std::uint32_t _readerWeight;
    std::uint32_t _totalWeight;
};

class PriorityTicketHolder final : public SchedulingTicketHolder {
public:
    explicit PriorityTicketHolder(int numTickets, ServiceContext* serviceContext);

private:
    enum class QueueType : unsigned int {
        LowPriorityQueue = 0,
        NormalPriorityQueue = 1,
        QueueTypeSize = 2
    };

    void _dequeueWaitingThread() override final;

    Queue& _getQueueToUse(OperationContext* opCtx, const AdmissionContext* admCtx) override final;
};

/**
 * RAII-style movable token that gets generated when a ticket is acquired and is automatically
 * released when going out of scope.
 */
class Ticket {
    friend class TicketHolder;
    friend class ReaderWriterTicketHolder;
    friend class TicketHolderWithQueueingStats;
    friend class SemaphoreTicketHolder;
    friend class FifoTicketHolder;
    friend class SchedulingTicketHolder;

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
            _ticketholder->_release(_admissionContext);
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
