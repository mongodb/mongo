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

#include <mutex>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/db/repl/oplog_buffer_blocking_queue.h"
#include "mongo/util/assert_util_core.h"

namespace mongo {
namespace repl {

namespace {

std::size_t getSingleDocumentSize(const BSONObj& doc) {
    return static_cast<size_t>(doc.objsize());
}

std::size_t getTotalDocumentSize(OplogBuffer::Batch::const_iterator begin,
                                 OplogBuffer::Batch::const_iterator end) {
    std::size_t totalSize = 0;
    for (auto it = begin; it != end; ++it) {
        totalSize += getSingleDocumentSize(*it);
    }
    return totalSize;
}

}  // namespace

OplogBufferBlockingQueue::OplogBufferBlockingQueue(std::size_t maxSize)
    : OplogBufferBlockingQueue(maxSize, nullptr, Options()) {}
OplogBufferBlockingQueue::OplogBufferBlockingQueue(std::size_t maxSize,
                                                   Counters* counters,
                                                   Options options)
    : _maxSize(maxSize), _counters(counters), _options(std::move(options)) {}

void OplogBufferBlockingQueue::startup(OperationContext*) {
    invariant(!_isShutdown);
    // Update server status metric to reflect the current oplog buffer's max size.
    if (_counters) {
        _counters->setMaxSize(getMaxSize());
    }
}

void OplogBufferBlockingQueue::shutdown(OperationContext* opCtx) {
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _isShutdown = true;
        if (_options.clearOnShutdown) {
            _clear_inlock(lk);
        } else {
            _notFullCV.notify_one();
            _notEmptyCV.notify_one();
        }
    }

    if (_options.clearOnShutdown && _counters) {
        _counters->clear();
    }
}

void OplogBufferBlockingQueue::push(OperationContext*,
                                    Batch::const_iterator begin,
                                    Batch::const_iterator end,
                                    boost::optional<std::size_t> bytes) {
    if (begin == end) {
        return;
    }

    // Get the total byte size if caller did not provide one.
    //
    // It is the caller's responsibility to make sure that the total byte
    // size provided is equal to the sum of all document sizes, we do not
    // verify it here.
    auto size = bytes ? *bytes : getTotalDocumentSize(begin, end);
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
        _queue.insert(_queue.end(), begin, end);
        _curSize += size;

        if (startedEmpty) {
            _notEmptyCV.notify_one();
        }
    }

    if (_counters) {
        _counters->incrementN(count, size);
    }
}

void OplogBufferBlockingQueue::waitForSpace(OperationContext*, std::size_t size) {
    stdx::unique_lock<Latch> lk(_mutex);
    _waitForSpace_inlock(lk, size);
}

bool OplogBufferBlockingQueue::isEmpty() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return _queue.empty();
}

std::size_t OplogBufferBlockingQueue::getMaxSize() const {
    return _maxSize;
}

std::size_t OplogBufferBlockingQueue::getSize() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return _curSize;
}

std::size_t OplogBufferBlockingQueue::getCount() const {
    stdx::lock_guard<Latch> lk(_mutex);
    return _queue.size();
}

void OplogBufferBlockingQueue::clear(OperationContext*) {
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _clear_inlock(lk);
    }

    if (_counters) {
        _counters->clear();
    }
}

bool OplogBufferBlockingQueue::tryPop(OperationContext*, Value* value) {
    {
        stdx::lock_guard<Latch> lk(_mutex);

        if (_queue.empty()) {
            return false;
        }

        *value = _queue.front();
        _queue.pop_front();
        _curSize -= getSingleDocumentSize(*value);
        invariant(_curSize >= 0);

        // Only notify producer if there is a waiting producer and enough space available.
        if (_waitSize > 0 && _curSize + _waitSize <= _maxSize) {
            _notFullCV.notify_one();
        }
    }

    if (_counters) {
        _counters->decrement(*value);
    }

    return true;
}

bool OplogBufferBlockingQueue::waitForDataFor(Milliseconds waitDuration,
                                              Interruptible* interruptible) {
    stdx::unique_lock<Latch> lk(_mutex);

    interruptible->waitForConditionOrInterruptFor(_notEmptyCV, lk, waitDuration, [this] {
        return !_queue.empty() || _drainMode || _isShutdown;
    });

    return !_queue.empty();
}

bool OplogBufferBlockingQueue::waitForDataUntil(Date_t deadline, Interruptible* interruptible) {
    stdx::unique_lock<Latch> lk(_mutex);

    interruptible->waitForConditionOrInterruptUntil(
        _notEmptyCV, lk, deadline, [this] { return !_queue.empty() || _drainMode || _isShutdown; });

    return !_queue.empty();
}

bool OplogBufferBlockingQueue::peek(OperationContext*, Value* value) {
    stdx::lock_guard<Latch> lk(_mutex);

    if (_queue.empty()) {
        return false;
    }
    *value = _queue.front();

    return true;
}

boost::optional<OplogBuffer::Value> OplogBufferBlockingQueue::lastObjectPushed(
    OperationContext*) const {
    stdx::lock_guard<Latch> lk(_mutex);
    if (_queue.empty()) {
        return {};
    }

    return _queue.back();
}

void OplogBufferBlockingQueue::enterDrainMode() {
    stdx::lock_guard<Latch> lk(_mutex);
    _drainMode = true;
    _notEmptyCV.notify_one();
}

void OplogBufferBlockingQueue::exitDrainMode() {
    stdx::lock_guard<Latch> lk(_mutex);
    _drainMode = false;
}

bool OplogBufferBlockingQueue::inDrainModeAndEmpty() {
    stdx::lock_guard<Latch> lk(_mutex);
    return _drainMode && _queue.empty();
}

void OplogBufferBlockingQueue::_waitForSpace_inlock(stdx::unique_lock<Latch>& lk,
                                                    std::size_t size) {
    invariant(size > 0);
    invariant(!_waitSize);

    while (_curSize + size > _maxSize && !_isShutdown) {
        // We only support one concurrent producer.
        _waitSize = size;
        _notFullCV.wait(lk);
        _waitSize = 0;
    }
}

void OplogBufferBlockingQueue::_clear_inlock(WithLock lk) {
    _queue = {};
    _curSize = 0;
    _notFullCV.notify_one();
    _notEmptyCV.notify_one();
}

}  // namespace repl
}  // namespace mongo
