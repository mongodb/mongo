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

#include "mongo/tools/workload_simulation/event_queue.h"

#include "mongo/stdx/chrono.h"

namespace mongo::workload_simulation {

EventQueue::EventQueue(TickSourceMock<Nanoseconds>& tickSource, std::function<size_t()> actorCount)
    : _tickSource(tickSource), _actorCount(actorCount) {}

void EventQueue::start() {
    _draining.store(false);
    _timer = stdx::thread([this]() {
        while (!_stopping.load()) {
            _processQueues();
        }
    });
}

void EventQueue::prepareStop() {
    _draining.store(true);
}

void EventQueue::stop() {
    if (_timer.joinable()) {
        _stopping.store(true);
        _timer.join();
    }
}

void EventQueue::clear() {
    while (!_eventQueue.empty()) {
        std::get<1>(_eventQueue.top())->emplaceValue();
        _eventQueue.pop();
    }
    while (!_observerQueue.empty()) {
        std::get<1>(_observerQueue.top())->emplaceValue();
        _observerQueue.pop();
    }
}

template <typename Duration>
bool EventQueue::wait_for(Duration d, WaitType t) {
    if (_draining.load()) {
        return false;
    }

    auto promise = std::make_unique<promise_type>();
    auto notifier = promise->getFuture();
    {
        stdx::lock_guard lk{_mutex};
        auto currentTime = _tickSource.getTicks();
        invariant(currentTime > 0);
        auto wakeTime = currentTime + duration_cast<Nanoseconds>(d).count();
        invariant(wakeTime > currentTime);
        switch (t) {
            case WaitType::Event:
                _eventQueue.push(
                    std::make_tuple(wakeTime, std::move(promise), std::string{getThreadName()}));
                break;
            case WaitType::Observer:
                _observerQueue.push(
                    std::make_tuple(wakeTime, std::move(promise), std::string{getThreadName()}));
        }
    }
    _cv.notify_one();
    (void)notifier.getNoThrow();
    return true;
}
template bool EventQueue::wait_for<Nanoseconds>(Nanoseconds, WaitType);
template bool EventQueue::wait_for<Microseconds>(Microseconds, WaitType);
template bool EventQueue::wait_for<Milliseconds>(Milliseconds, WaitType);
template bool EventQueue::wait_for<Seconds>(Seconds, WaitType);

void EventQueue::_processQueues() {
    stdx::unique_lock lk{_mutex};
    if (!_haveEventToProcess()) {
        if (!_cv.wait_for(
                lk, stdx::chrono::milliseconds(1), [this]() { return _haveEventToProcess(); })) {
            return;
        }
    }

    auto batchTime = _nextEventTime();
    while (_haveEventAtOrBeforeTime(batchTime)) {
        auto [event, queue] = _nextEventToProcess();

        auto currentTime = _tickSource.getTicks();
        auto eventTime = std::get<0>(*event);
        if (eventTime > currentTime) {
            _tickSource.advance(Nanoseconds{eventTime - currentTime});
        }

        std::get<1>(*event)->emplaceValue();
        queue->pop();
    }
}

bool EventQueue::_haveEventToProcess() {
    if (_draining.load() && (!_eventQueue.empty() || !_observerQueue.empty())) {
        return true;
    }

    if (_eventQueue.size() >= _actorCount()) {
        return true;
    }

    return false;
}

bool EventQueue::_haveEventAtOrBeforeTime(time_type t) {
    return (!_eventQueue.empty() && std::get<0>(_eventQueue.top()) <= t) ||
        (!_observerQueue.empty() && std::get<0>(_observerQueue.top()) <= t);
}

EventQueue::time_type EventQueue::_nextEventTime() {
    if (_eventQueue.empty()) {
        return std::numeric_limits<time_type>::max();
    }
    return std::get<0>(_eventQueue.top());
}

std::pair<const EventQueue::storage_type*, EventQueue::queue_type*>
EventQueue::_nextEventToProcess() {
    bool useEventQueue = true;
    if (_eventQueue.empty() ||
        (!_observerQueue.empty() && time_greater{}(_eventQueue.top(), _observerQueue.top()))) {
        useEventQueue = false;
    }

    if (useEventQueue) {
        return std::make_pair(&_eventQueue.top(), &_eventQueue);
    }
    return std::make_pair(&_observerQueue.top(), &_observerQueue);
}

}  // namespace mongo::workload_simulation
