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

#include "mongo/stdx/condition_variable.h"
#include <queue>
#include <thread>

#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/tick_source_mock.h"

namespace mongo::workload_simulation {

/**
 * This class coordinates event processing and clock advancement for a simulated workload.
 */
class EventQueue {
public:
    using time_type = TickSource::Tick;
    enum class WaitType { Event, Observer };

private:
    using promise_type = SharedPromise<void>;
    using storage_type = std::tuple<time_type, std::unique_ptr<promise_type>, std::string>;
    struct time_greater {
        bool operator()(const storage_type& lhs, const storage_type& rhs) const {
            return std::get<0>(lhs) > std::get<0>(rhs);
        }
    };
    using queue_type = std::priority_queue<storage_type, std::vector<storage_type>, time_greater>;

public:
    /**
     * @param tickSource    Mock clock, which should only be advanced by this 'EventQueue'.
     * @param actorCount    During normal operation, the processing thread will block until it
     *                      contains at least as many events as the number returned by
     *                      'actorCount()'. The return value of this function should reflect the
     *                      current state of the system (e.g. it should be dynamic if the number of
     *                      actors active at a given time can change).
     */
    EventQueue(TickSourceMock<Nanoseconds>& tickSource, std::function<size_t()> actorCount);

    /**
     * Begin normal operation. The processing thread will wait until it has at least 'actorCount()'
     * Event-type operations queued. Once it does, it will process all operations that are queued
     * with times at most that of the minimum Event-type operation in time order, advancing the
     * clock to the current operation time.
     */
    void start();

    /**
     * End normal operation. The queue will now drain events immediately, rather than waiting for
     * 'actorCount()' events to accrue. Subsequent calls to 'wait_for' will return immediately.
     */
    void prepareStop();

    /**
     * Stops the processing thread.
     */
    void stop();

    /**
     * Remove any remaining requests from the queue immediately.
     */
    void clear();

    /**
     * Wait for the clock to advance a specified duration. Blocking.
     *
     * @param d     Duration to wait.
     * @param t     Operation type. An 'Event' will count towards the 'actorCount()' and potentially
     *              advance the clock. An 'Observer' will not, and will only be processed if an
     *              'Event' advances the clock sufficiently.
     * @return      Whether it actually waited. May return 'false' if 'prepareStop' was called.
     */
    template <typename Duration>
    bool wait_for(Duration d, WaitType t);

private:
    void _processQueues();
    bool _haveEventToProcess();
    bool _haveEventAtOrBeforeTime(time_type t);
    time_type _nextEventTime();
    std::pair<const storage_type*, queue_type*> _nextEventToProcess();

    mutable stdx::mutex _mutex;
    mutable stdx::condition_variable _cv;

    TickSourceMock<Nanoseconds>& _tickSource;
    std::function<size_t()> _actorCount;
    queue_type _eventQueue;
    queue_type _observerQueue;
    stdx::thread _timer;
    AtomicWord<bool> _stopping{false};
    AtomicWord<bool> _draining{false};
};

}  // namespace mongo::workload_simulation
