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

#include <list>

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
    virtual bool empty() const = 0;
    virtual void push(std::shared_ptr<TicketWaiter>) = 0;
    virtual std::shared_ptr<TicketWaiter> pop() = 0;
};

/**
 * The FifoTicketQueue is a simple FIFO queue where new waiters are placed at the end and the oldest
 * waiters are removed first.
 */
class FifoTicketQueue : public TicketQueue {
public:
    bool empty() const {
        return _queue.empty();
    }

    void push(std::shared_ptr<TicketWaiter> val) {
        _queue.push_back(std::move(val));
    }

    std::shared_ptr<TicketWaiter> pop() {
        auto front = std::move(_queue.front());
        _queue.pop_front();
        return front;
    }

private:
    std::list<std::shared_ptr<TicketWaiter>> _queue;
};

/**
 * A TicketPool holds tickets and queues waiters in the provided TicketQueue. The TicketPool
 * attempts to emulate a semaphore with a custom queueing policy.
 *
 * All public functions are thread-safe except where explicitly stated otherwise.
 */
class TicketPool {
public:
    TicketPool(int numTickets, std::unique_ptr<TicketQueue> queue);

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
     * If there are queued operations, releases a ticket and returns true. Otherwise, does nothing
     * and returns false.
     */
    bool releaseIfWaiters();

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
    TicketQueue* getQueue() {
        return _waiters.get();
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
    std::unique_ptr<TicketQueue> _waiters;
};
}  // namespace mongo
