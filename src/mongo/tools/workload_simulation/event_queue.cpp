// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/tools/workload_simulation/event_queue.h"

#include <chrono>

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
        std::lock_guard lk{_mutex};
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
    std::unique_lock lk{_mutex};
    if (!_haveEventToProcess()) {
        if (!_cv.wait_for(
                lk, std::chrono::milliseconds(1), [this]() { return _haveEventToProcess(); })) {
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
