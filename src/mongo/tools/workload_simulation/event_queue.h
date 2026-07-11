// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/atomic.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"
#include "mongo/util/tick_source_mock.h"

#include <queue>
#include <thread>

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

    mutable std::mutex _mutex;
    mutable stdx::condition_variable _cv;

    TickSourceMock<Nanoseconds>& _tickSource;
    std::function<size_t()> _actorCount;
    queue_type _eventQueue;
    queue_type _observerQueue;
    stdx::thread _timer;
    Atomic<bool> _stopping{false};
    Atomic<bool> _draining{false};
};

}  // namespace mongo::workload_simulation
