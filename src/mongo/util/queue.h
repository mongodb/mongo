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

#include "mongo/stdx/chrono.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/hierarchical_acquisition.h"

#include <functional>
#include <limits>
#include <queue>

#include <boost/optional.hpp>

namespace mongo {

/**
 * Simple blocking queue with optional max size (by count or custom sizing function).
 * A custom sizing function can optionally be given.  By default the getSize function
 * returns 1 for each item, resulting in size equaling the number of items queued.
 *
 * Note that use of this class is deprecated.  This class only works with a single consumer and
 * a single producer.
 */
template <typename T>
class BlockingQueue {
    BlockingQueue(const BlockingQueue&) = delete;
    BlockingQueue& operator=(const BlockingQueue&) = delete;

public:
    using GetSizeFn = std::function<size_t(const T&)>;

    BlockingQueue() : BlockingQueue(std::numeric_limits<std::size_t>::max()) {}
    BlockingQueue(size_t size) : BlockingQueue(size, [](const T&) { return 1; }) {}
    BlockingQueue(size_t size, GetSizeFn f) : _maxSize(size), _getSize(f) {}

    /**
     * Returns when enough space is available.
     *
     * NOTE: Should only be used in a single producer case.
     */
    void waitForSpace(size_t size) {
        stdx::unique_lock<stdx::mutex> lk(_lock);
        _waitForSpace_inlock(size, lk);
    }

    /**
     * Pushes all entries.
     *
     * If enough space is not available, this method will block.
     *
     * NOTE: Should only be used in a single producer case.
     */
    template <typename Iterator>
    void pushAllBlocking(Iterator begin, Iterator end) {
        if (begin == end) {
            return;
        }

        size_t size = 0;
        for (auto i = begin; i != end; ++i) {
            size += _getSize(*i);
        }
        // Block until enough space is available.
        waitForSpace(size);

        stdx::unique_lock<stdx::mutex> lk(_lock);
        const auto startedEmpty = _queue.empty();
        _clearing = false;

        auto pushOne = [this](const T& obj) {
            size_t tSize = _getSize(obj);
            _queue.push(obj);
            _currentSize += tSize;
        };
        std::for_each(begin, end, pushOne);

        if (startedEmpty) {
            _cvNoLongerEmpty.notify_one();
        }
    }

    bool empty() const {
        stdx::lock_guard<stdx::mutex> lk(_lock);
        return _queue.empty();
    }

    /**
     * The size as measured by the size function. Default to counting each item
     */
    size_t size() const {
        stdx::lock_guard<stdx::mutex> lk(_lock);
        return _currentSize;
    }

    /**
     * The max size for this queue
     */
    size_t maxSize() const {
        return _maxSize;
    }

    /**
     * The number/count of items in the queue ( _queue.size() )
     */
    size_t count() const {
        stdx::lock_guard<stdx::mutex> lk(_lock);
        return _queue.size();
    }

    void clear() {
        stdx::lock_guard<stdx::mutex> lk(_lock);
        _clearing = true;
        _queue = std::queue<T>();
        _currentSize = 0;
        _cvNoLongerFull.notify_one();
        _cvNoLongerEmpty.notify_one();
    }

    bool tryPop(T& t) {
        stdx::lock_guard<stdx::mutex> lk(_lock);
        if (_queue.empty())
            return false;

        t = _queue.front();
        _queue.pop();
        _currentSize -= _getSize(t);
        _cvNoLongerFull.notify_one();

        return true;
    }

    T blockingPop() {
        stdx::unique_lock<stdx::mutex> lk(_lock);
        _clearing = false;
        while (_queue.empty() && !_clearing)
            _cvNoLongerEmpty.wait(lk);
        if (_clearing) {
            return T{};
        }

        T t = _queue.front();
        _queue.pop();
        _currentSize -= _getSize(t);
        _cvNoLongerFull.notify_one();

        return t;
    }


    /**
     * blocks waiting for an object until maxSecondsToWait passes
     * if got one, return true and set in t
     * otherwise return false and t won't be changed
     */
    bool blockingPop(T& t, int maxSecondsToWait) {
        using namespace stdx::chrono;
        const auto deadline = system_clock::now() + seconds(maxSecondsToWait);
        stdx::unique_lock<stdx::mutex> lk(_lock);
        _clearing = false;
        while (_queue.empty() && !_clearing) {
            if (stdx::cv_status::timeout == _cvNoLongerEmpty.wait_until(lk, deadline))
                return false;
        }

        if (_clearing) {
            return false;
        }
        t = _queue.front();
        _queue.pop();
        _currentSize -= _getSize(t);
        _cvNoLongerFull.notify_one();
        return true;
    }

    // Obviously, this should only be used when you have
    // only one consumer
    bool blockingPeek(T& t, int maxSecondsToWait) {
        using namespace stdx::chrono;
        const auto deadline = system_clock::now() + seconds(maxSecondsToWait);
        stdx::unique_lock<stdx::mutex> lk(_lock);
        _clearing = false;
        while (_queue.empty() && !_clearing) {
            if (stdx::cv_status::timeout == _cvNoLongerEmpty.wait_until(lk, deadline))
                return false;
        }
        if (_clearing) {
            return false;
        }
        t = _queue.front();
        return true;
    }

    // Obviously, this should only be used when you have
    // only one consumer
    bool peek(T& t) {
        stdx::unique_lock<stdx::mutex> lk(_lock);
        if (_queue.empty()) {
            return false;
        }

        t = _queue.front();
        return true;
    }

    /**
     * Returns the item most recently added to the queue or nothing if the queue is empty.
     */
    boost::optional<T> lastObjectPushed() const {
        stdx::unique_lock<stdx::mutex> lk(_lock);
        if (_queue.empty()) {
            return {};
        }

        return {_queue.back()};
    }

private:
    /**
     * Returns when enough space is available.
     */
    void _waitForSpace_inlock(size_t size, stdx::unique_lock<stdx::mutex>& lk) {
        while (_currentSize + size > _maxSize) {
            _cvNoLongerFull.wait(lk);
        }
    }

    mutable stdx::mutex _lock;
    std::queue<T> _queue;
    const size_t _maxSize;
    size_t _currentSize = 0;
    GetSizeFn _getSize;
    bool _clearing = false;

    stdx::condition_variable _cvNoLongerFull;
    stdx::condition_variable _cvNoLongerEmpty;
};
}  // namespace mongo
