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

#include "mongo/db/repl/oplog_buffer_blocking_queue.h"

#include "mongo/util/assert_util.h"

#include <mutex>
#include <vector>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace repl {

namespace {

std::size_t getDocumentOpCount(const BSONObj& doc) {
    if (doc[OplogEntry::kOpTypeFieldName].String() != "c"_sd) {
        return 1U;
    }

    // Get the number of operations enclosed in 'applyOps'. Use The 'count'
    // field if it exists, otherwise fallback to use BSONObj::nFields().
    auto obj = doc["o"].Obj();
    auto applyOps = obj[ApplyOpsCommandInfoBase::kOperationsFieldName];

    if (!applyOps.ok()) {
        return 1U;
    }

    auto count = obj.getIntField(ApplyOpsCommandInfoBase::kCountFieldName);
    if (count > 0) {
        return std::size_t(count);
    }
    count = applyOps.Obj().nFields();

    return count > 0 ? std::size_t(count) : 1U;
}

OplogBuffer::Cost getDocumentCost(const BSONObj& doc) {
    return {static_cast<std::size_t>(doc.objsize()), getDocumentOpCount(doc)};
}

}  // namespace

OplogBufferBlockingQueue::OplogBufferBlockingQueue(std::size_t maxSize, std::size_t maxCount)
    : OplogBufferBlockingQueue(maxSize, maxCount, nullptr, Options()) {}
OplogBufferBlockingQueue::OplogBufferBlockingQueue(std::size_t maxSize,
                                                   std::size_t maxCount,
                                                   Counters* counters,
                                                   Options options)
    : _maxSize(maxSize), _maxCount(maxCount), _counters(counters), _options(std::move(options)) {
    invariant(maxSize > 0 && maxCount > 0);
}

void OplogBufferBlockingQueue::startup(OperationContext*) {
    invariant(!_isShutdown);
    // Update server status metric to reflect the current oplog buffer's max size.
    if (_counters) {
        _counters->setMaxSize(_maxSize);
        _counters->setMaxCount(_maxCount);
    }
}

void OplogBufferBlockingQueue::shutdown(OperationContext* opCtx) {
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _isShutdown = true;
        if (_options.clearOnShutdown) {
            _clear(lk);
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
                                    boost::optional<const Cost&> cost) {
    if (begin == end) {
        return;
    }

    // If caller knows the cost, use it, otherwise calculate the cost.
    if (cost) {
        _push(begin, end, *cost);
        return;
    }

    Cost sumCost;
    Cost maxCost{_maxSize / 2, _maxCount / 2};
    auto lower = begin;

    // The cost of the batch could be larger than the limit, break it into
    // smaller batches.
    for (auto upper = begin; upper != end; ++upper) {
        if (sumCost.size < maxCost.size && sumCost.count < maxCost.count) {
            auto docCost = getDocumentCost(*upper);
            sumCost.size += docCost.size;
            sumCost.count += docCost.count;
            continue;
        }
        _push(lower, upper, sumCost);
        lower = upper;
        auto docCost = getDocumentCost(*upper);
        sumCost.size = docCost.size;
        sumCost.count = docCost.count;
    }
    _push(lower, end, sumCost);
}

void OplogBufferBlockingQueue::_push(Batch::const_iterator begin,
                                     Batch::const_iterator end,
                                     const Cost& cost) {
    invariant(begin != end);
    {
        stdx::unique_lock<stdx::mutex> lk(_mutex);

        // Block until enough space is available.
        invariant(!_drainMode);
        _waitForSpace(lk, cost);

        // Do not push anything if already shutdown.
        if (_isShutdown) {
            return;
        }

        bool startedEmpty = _queue.empty();
        _queue.insert(_queue.end(), begin, end);
        _curSize += cost.size;
        _curCount += cost.count;

        if (startedEmpty) {
            _notEmptyCV.notify_one();
        }
    }

    if (_counters) {
        _counters->incrementN(cost.count, cost.size);
    }
}

void OplogBufferBlockingQueue::waitForSpace(OperationContext*, const Cost& cost) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _waitForSpace(lk, cost);
}

bool OplogBufferBlockingQueue::isEmpty() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _queue.empty();
}

std::size_t OplogBufferBlockingQueue::getSize() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _curSize;
}

std::size_t OplogBufferBlockingQueue::getCount() const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _curCount;
}

void OplogBufferBlockingQueue::clear(OperationContext*) {
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _clear(lk);
    }

    if (_counters) {
        _counters->clear();
    }
}

bool OplogBufferBlockingQueue::tryPop(OperationContext*, Value* value) {
    Cost cost;
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);

        if (_queue.empty()) {
            return false;
        }

        *value = _queue.front();
        _queue.pop_front();
        cost = getDocumentCost(*value);
        _curSize -= cost.size;
        _curCount -= cost.count;
        invariant(_curSize >= 0 && _curCount >= 0);

        // Only notify producer if there is a waiting producer and enough space available.
        if (_waitSize || _waitCount) {
            if (_queue.empty() ||
                ((_curSize + _waitSize <= _maxSize) && (_curCount + _waitCount <= _maxCount))) {
                _notFullCV.notify_one();
            }
        }
    }

    if (_counters) {
        _counters->decrementN(cost.count, cost.size);
    }

    return true;
}

bool OplogBufferBlockingQueue::waitForDataFor(Milliseconds waitDuration,
                                              Interruptible* interruptible) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    interruptible->waitForConditionOrInterruptFor(_notEmptyCV, lk, waitDuration, [this] {
        return !_queue.empty() || _drainMode || _isShutdown;
    });

    return !_queue.empty();
}

bool OplogBufferBlockingQueue::waitForDataUntil(Date_t deadline, Interruptible* interruptible) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    interruptible->waitForConditionOrInterruptUntil(
        _notEmptyCV, lk, deadline, [this] { return !_queue.empty() || _drainMode || _isShutdown; });

    return !_queue.empty();
}

bool OplogBufferBlockingQueue::peek(OperationContext*, Value* value) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);

    if (_queue.empty()) {
        return false;
    }
    *value = _queue.front();

    return true;
}

boost::optional<OplogBuffer::Value> OplogBufferBlockingQueue::lastObjectPushed(
    OperationContext*) const {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_queue.empty()) {
        return {};
    }

    return _queue.back();
}

void OplogBufferBlockingQueue::enterDrainMode() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _drainMode = true;
    _notEmptyCV.notify_one();
}

void OplogBufferBlockingQueue::exitDrainMode() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _drainMode = false;
}

void OplogBufferBlockingQueue::_waitForSpace(stdx::unique_lock<stdx::mutex>& lk, const Cost& cost) {
    invariant(cost.size > 0 && cost.count > 0);
    invariant(!_waitSize && !_waitCount);

    // Allow any cost if queue is empty, caller should do appropriate batching.
    while ((_curSize + cost.size > _maxSize || _curCount + cost.count > _maxCount) &&
           !_queue.empty() && !_isShutdown) {
        // We only support one concurrent producer.
        _waitSize = cost.size;
        _waitCount = cost.count;
        _notFullCV.wait(lk);
        _waitSize = 0;
        _waitCount = 0;
    }
}

void OplogBufferBlockingQueue::_clear(WithLock lk) {
    _queue = {};
    _curSize = 0;
    _curCount = 0;
    _notFullCV.notify_one();
    _notEmptyCV.notify_one();
}

}  // namespace repl
}  // namespace mongo
