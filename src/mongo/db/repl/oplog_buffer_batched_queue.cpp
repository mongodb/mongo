/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/repl/oplog_buffer_batched_queue.h"

namespace mongo {
namespace repl {

OplogBufferBatchedQueue::OplogBufferBatchedQueue(std::size_t maxSize)
    : OplogBufferBatchedQueue(maxSize, nullptr) {}

OplogBufferBatchedQueue::OplogBufferBatchedQueue(std::size_t maxSize, Counters* counters)
    : _maxSize(maxSize), _counters(counters) {}

void OplogBufferBatchedQueue::startup(OperationContext*) {
    invariant(!_isShutdown);
    // Update server status metric to reflect the current oplog buffer's max size.
    if (_counters) {
        _counters->setMaxSize(getMaxSize());
    }
}

void OplogBufferBatchedQueue::shutdown(OperationContext* opCtx) {
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _isShutdown = true;
        _clear_inlock(lk);
    }

    if (_counters) {
        _counters->clear();
    }
}

void OplogBufferBatchedQueue::push(OperationContext*,
                                   Batch::const_iterator begin,
                                   Batch::const_iterator end,
                                   boost::optional<std::size_t> bytes) {
    if (begin == end) {
        return;
    }

    invariant(bytes);
    auto size = *bytes;
    auto count = std::distance(begin, end);

    {
        stdx::unique_lock<Latch> lk(_mutex);

        // Block until enough space is available.
        invariant(!_drainMode);
        _waitForSpace_inlock(lk, size);

        // Do not push anything if already shutdown.
        if (_isShutdown) {
            return;
        }

        bool startedEmpty = _queue.empty();
        _queue.emplace_back(begin, end, size);
        _curSize += size;
        _curCount += count;

        if (startedEmpty) {
            _notEmptyCV.notify_one();
        }
    }

    if (_counters) {
        _counters->incrementN(count, size);
    }
}

void OplogBufferBatchedQueue::waitForSpace(OperationContext* opCtx, std::size_t size) {
    stdx::unique_lock<Latch> lk(_mutex);
    _waitForSpace_inlock(lk, size);
}

bool OplogBufferBatchedQueue::isEmpty() const {
    stdx::lock_guard<Latch> lk(_mutex);
    invariant(!_curCount == _queue.empty());
    return !_curCount;
}

std::size_t OplogBufferBatchedQueue::getMaxSize() const {
    return _maxSize;
}

std::size_t OplogBufferBatchedQueue::getSize() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return _curSize;
}

std::size_t OplogBufferBatchedQueue::getCount() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return _curCount;
}

void OplogBufferBatchedQueue::clear(OperationContext*) {
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _clear_inlock(lk);
    }

    if (_counters) {
        _counters->clear();
    }
}

bool OplogBufferBatchedQueue::tryPopBatch(OperationContext* opCtx, OplogBatch<Value>* batch) {
    {
        stdx::lock_guard<Latch> lk(_mutex);

        if (_queue.empty()) {
            return false;
        }

        *batch = std::move(_queue.front());
        _queue.pop_front();
        _curSize -= batch->byteSize();
        _curCount -= batch->count();

        // Only notify producer if there is a waiting producer and enough space available.
        if (_waitSize > 0 && _curSize + _waitSize <= _maxSize) {
            _notFullCV.notify_one();
        }
    }

    if (_counters) {
        _counters->decrementN(batch->count(), batch->byteSize());
    }

    return true;
}

bool OplogBufferBatchedQueue::waitForDataFor(Milliseconds waitDuration,
                                             Interruptible* interruptible) {
    stdx::unique_lock<Latch> lk(_mutex);

    interruptible->waitForConditionOrInterruptFor(_notEmptyCV, lk, waitDuration, [this] {
        return !_queue.empty() || _drainMode || _isShutdown;
    });

    return !_queue.empty();
}

bool OplogBufferBatchedQueue::waitForDataUntil(Date_t deadline, Interruptible* interruptible) {
    stdx::unique_lock<Latch> lk(_mutex);

    interruptible->waitForConditionOrInterruptUntil(
        _notEmptyCV, lk, deadline, [this] { return !_queue.empty() || _drainMode || _isShutdown; });

    return !_queue.empty();
}

void OplogBufferBatchedQueue::enterDrainMode() {
    stdx::lock_guard<Latch> lk(_mutex);
    _drainMode = true;
    _notEmptyCV.notify_one();
}

void OplogBufferBatchedQueue::exitDrainMode() {
    stdx::lock_guard<Latch> lk(_mutex);
    _drainMode = false;
}

bool OplogBufferBatchedQueue::inDrainMode() {
    stdx::lock_guard<Latch> lk(_mutex);
    return _drainMode;
}

void OplogBufferBatchedQueue::_waitForSpace_inlock(stdx::unique_lock<Latch>& lk, std::size_t size) {
    invariant(size > 0);
    invariant(!_waitSize);

    while (_curSize + size > _maxSize && !_isShutdown) {
        // We only support one concurrent producer.
        _waitSize = size;
        _notFullCV.wait(lk);
        _waitSize = 0;
    }
}

void OplogBufferBatchedQueue::_clear_inlock(WithLock lk) {
    _queue = {};
    _curSize = 0;
    _curCount = 0;
    _notFullCV.notify_one();
    _notEmptyCV.notify_one();
}

}  // namespace repl
}  // namespace mongo
