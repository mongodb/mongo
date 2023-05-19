/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <queue>

#include "mongo/platform/atomic_word.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/time_support.h"

namespace mongo {

/**
 * A ticket waiter represents an operation that queues when no tickets are available.
 */
struct TicketWaiter {
    enum State : uint32_t {
        // This is the initial state. May transition to only Acquired or TimedOut.
        Waiting = 0,
        // A releaser will set the waiter to the Acquired state when a ticket is available. This is
        // a terminal state.
        Acquired,
        // The waiter will transition to this state when it times out. Releasers will not give
        // tickets to waiters in the TimedOut state. This is a terminal state.
        TimedOut,
    };
    AtomicWord<uint32_t> futexWord{Waiting};

    // Only valid to dereference when in the Waiting state and while holding the queue lock.
    AdmissionContext* context{nullptr};
};

/**
 * A TicketQueue is an interface that represents a queue of waiters whose ordering is
 * implementation-defined.
 */
class TicketQueue {
public:
    virtual ~TicketQueue(){};
    virtual void push(std::shared_ptr<TicketWaiter>) = 0;
    virtual std::shared_ptr<TicketWaiter> pop() = 0;
};

/**
 * The FifoTicketQueue is a simple FIFO queue where new waiters are placed at the end and the oldest
 * waiters are removed first.
 */
class FifoTicketQueue : public TicketQueue {
public:
    void push(std::shared_ptr<TicketWaiter> val) {
        _queue.push(std::move(val));
    }

    std::shared_ptr<TicketWaiter> pop() {
        if (_queue.empty()) {
            return nullptr;
        }
        auto front = std::move(_queue.front());
        _queue.pop();
        return front;
    }

private:
    std::queue<std::shared_ptr<TicketWaiter>> _queue;
};

/**
 * This SimplePriorityTicketQueue implements a queue policy that separates normal and low priority
 * operations into separate queues. Normal priority operations are always scheduled ahead of low
 * priority ones, except when a positive lowPriorityBypassThreshold is provided. This parameter
 * specifies how often a waiting low-priority operation should skip the queue and be scheduled ahead
 * of waiting normal priority operations.
 */
class SimplePriorityTicketQueue : public TicketQueue {
public:
    SimplePriorityTicketQueue(int lowPriorityBypassThreshold)
        : _lowPriorityBypassThreshold(lowPriorityBypassThreshold) {}

    void push(std::shared_ptr<TicketWaiter> val) final {
        if (val->context->getPriority() == AdmissionContext::Priority::kLow) {
            _low.push(std::move(val));
            return;
        }
        invariant(val->context->getPriority() == AdmissionContext::Priority::kNormal);
        _normal.push(std::move(val));
    }

    std::shared_ptr<TicketWaiter> pop() final {
        auto normalQueued = !_normal.empty();
        auto lowQueued = !_low.empty();
        if (!normalQueued && !lowQueued) {
            return nullptr;
        }
        if (normalQueued && lowQueued && _lowPriorityBypassThreshold.load() > 0 &&
            _lowPriorityBypassCount.fetchAndAdd(1) % _lowPriorityBypassThreshold.load() == 0) {
            auto front = std::move(_low.front());
            _low.pop();
            _expeditedLowPriorityAdmissions.addAndFetch(1);
            return front;
        }
        if (normalQueued) {
            auto front = std::move(_normal.front());
            _normal.pop();
            return front;
        }
        auto front = std::move(_low.front());
        _low.pop();
        return front;
    }

    /**
     * Number of times low priority operations are expedited for ticket admission over normal
     * priority operations.
     */
    std::int64_t expedited() const {
        return _expeditedLowPriorityAdmissions.loadRelaxed();
    }

    /**
     * Returns the number of times the low priority queue is bypassed in favor of dequeuing from the
     * normal priority queue when a ticket becomes available.
     */
    std::int64_t bypassed() const {
        return _lowPriorityBypassCount.loadRelaxed();
    }

    void updateLowPriorityAdmissionBypassThreshold(int32_t newBypassThreshold) {
        _lowPriorityBypassThreshold.store(newBypassThreshold);
    }

private:
    /**
     * Limits the number times the low priority queue is non-empty and bypassed in favor of the
     * normal priority queue for the next ticket admission.
     */
    AtomicWord<std::int32_t> _lowPriorityBypassThreshold;

    /**
     * Number of times ticket admission is expedited for low priority operations.
     */
    AtomicWord<std::int64_t> _expeditedLowPriorityAdmissions{0};

    /**
     * Counts the number of times normal operations are dequeued over operations queued in the low
     * priority queue. We explicitly use an unsigned type here because rollover is desired.
     */
    AtomicWord<std::uint64_t> _lowPriorityBypassCount{0};

    std::queue<std::shared_ptr<TicketWaiter>> _normal;
    std::queue<std::shared_ptr<TicketWaiter>> _low;
};


/**
 * A TicketPool holds tickets and queues waiters in the provided TicketQueue. The TicketPool
 * attempts to emulate a semaphore with a custom queueing policy.
 *
 * All public functions are thread-safe except where explicitly stated otherwise.
 */
template <class Queue>
class TicketPool {
public:
    template <typename... Args>
    TicketPool(int numTickets, Args&&... args)
        : _available(numTickets), _queued(0), _waiters(std::forward<Args>(args)...) {}

    /**
     * Attempt to acquire a ticket without blocking. Returns true if a ticket was granted.
     */
    bool tryAcquire();

    /**
     * Acquire a ticket until the provided deadline. Returns false on timeout, true otherwise.
     */
    bool acquire(AdmissionContext* admCtx, Date_t deadline);

    /**
     * Releases a ticket to the pool. Will will wake a waiter, if there are any queued operations.
     */
    void release();

    /**
     * Returns the number of tickets available.
     */
    int32_t available() const {
        return _available.load();
    }

    /**
     * Returns the number of queued waiters.
     */
    int32_t queued() const {
        return _queued.load();
    }

    /*
     * Provides direct access to the underlying queue. Callers must ensure they only use thread-safe
     * functions.
     */
    const Queue& getQueue() const {
        return _waiters;
    }

    Queue& getQueue() {
        return _waiters;
    }

private:
    /**
     * Attempt to give the ticket to a waiter, and otherwise the pool.
     */
    void _release();

    /**
     * Removes the next waiter from the queue. If there are no waiters, adds the ticket to the pool.
     * Ensures that no new waiters queue while this is happening.
     */
    std::shared_ptr<TicketWaiter> _popWaiterOrAddTicketToPool();

    AtomicWord<int32_t> _available;

    // This counter is redundant with the _waiters queue length, but provides the releaseIfWaiters()
    // a fast-path that avoids taking the queue mutex.
    AtomicWord<int32_t> _queued;

    // This mutex protects the _waiters queue by preventing items from being added and removed, but
    // does not protect the elements of the queue.
    Mutex _mutex = MONGO_MAKE_LATCH("TicketPool::_mutex");
    Queue _waiters;
};
}  // namespace mongo
