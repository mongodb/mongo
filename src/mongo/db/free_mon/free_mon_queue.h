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

#include <boost/optional.hpp>
#include <memory>
#include <mutex>
#include <queue>
#include <vector>

#include "mongo/db/free_mon/free_mon_message.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/time_support.h"

namespace mongo {

/**
 * Comparator for FreeMonMessage that will sort smallest deadlines at the beginning of a priority
 * queue. The std::priority_queue is a max-heap.
 */
struct FreeMonMessageGreater {
    bool operator()(const std::shared_ptr<FreeMonMessage>& left,
                    const std::shared_ptr<FreeMonMessage>& right) const {
        if (left->getDeadline() > right->getDeadline()) {
            return true;
        }

        if (left->getDeadline() == right->getDeadline()) {
            return left->getId() > right->getId();
        }

        return false;
    }
};

/**
 * Priority Queue with ability to remove items by filter.
 */
class FreeMonPriorityQueue {
public:
    bool empty() const {
        return _vector.empty();
    }

    /**
     * Return the message at the top of the priority queue.
     */
    std::shared_ptr<FreeMonMessage> top() const;

    /**
     * Pop the message at the top of the priority queue.
     */
    void pop();

    /**
     * Push a message into the priority queue.
     */
    void push(std::shared_ptr<FreeMonMessage> item);

    /**
     * Erase messages of a given type from the queue.
     */
    void eraseByType(FreeMonMessageType type);

private:
    // Using shared_ptr because std::pop_heap does not support move-only types
    std::vector<std::shared_ptr<FreeMonMessage>> _vector;
    FreeMonMessageGreater _comp;
};

/**
 * A multi-producer, single-consumer queue with deadlines.
 *
 * The smallest deadline sorts first. Messages with deadlines can be use as a timer mechanism.
 */
class FreeMonMessageQueue {
public:
    FreeMonMessageQueue(bool useCrankForTest = false) : _useCrank(useCrankForTest) {}

    /**
     * Enqueue a message and wake consumer if needed.
     *
     * Messages are dropped if the queue has been stopped.
     */
    void enqueue(std::shared_ptr<FreeMonMessage> msg);

    /**
     * Deque a message from the queue.
     *
     * Waits for a message to arrive. Returns boost::none if the queue has been stopped.
     */
    boost::optional<std::shared_ptr<FreeMonMessage>> dequeue(ClockSource* clockSource);

    /**
     * Stop the queue.
     */
    void stop();

    /**
     * Turn the crank of the message queue by ignoring deadlines for N messages.
     */
    void turnCrankForTest(size_t countMessagesToIgnore);

    /**
     * Deproritize the first message to force interleavings of messages.
     */
    void deprioritizeFirstMessageForTest(FreeMonMessageType type);

private:
    // Condition variable to signal consumer
    stdx::condition_variable _condvar;

    // Lock for condition variable and to protect state
    Mutex _mutex = MONGO_MAKE_LATCH("FreeMonMessageQueue::_mutex");

    // Indicates whether queue has been stopped.
    bool _stop{false};

    // Priority queue of messages with shortest deadline first
    FreeMonPriorityQueue _queue;

    // Use manual crank to process messages in-order instead of based on deadlines.
    bool _useCrank{false};

    // Stamp each message with a unique counter. This ensures that if two messages are queued with
    // the same deadline, FIFO is achieved.
    uint64_t _counter{0};

    // Number of messages to ignore
    size_t _countMessagesToIgnore{0};

    // Number of messages that have been ignored
    size_t _countMessagesIgnored{0};

    // Waitable result for testing
    std::unique_ptr<WaitableResult> _waitable;
};


}  // namespace mongo
