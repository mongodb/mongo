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

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <mutex>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>

#include "mongo/db/repl/oplog_buffer_proxy.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplication


namespace mongo {
namespace repl {

OplogBufferProxy::OplogBufferProxy(std::unique_ptr<OplogBuffer> target)
    : _target(std::move(target)) {
    invariant(_target);
}

OplogBuffer* OplogBufferProxy::getTarget() const {
    return _target.get();
}

void OplogBufferProxy::startup(OperationContext* opCtx) {
    _target->startup(opCtx);
}

void OplogBufferProxy::shutdown(OperationContext* opCtx) {
    {
        stdx::lock_guard<Latch> backLock(_lastPushedMutex);
        stdx::lock_guard<Latch> frontLock(_lastPeekedMutex);
        _lastPushed.reset();
        _lastPeeked.reset();
    }
    _target->shutdown(opCtx);
}

void OplogBufferProxy::push(OperationContext* opCtx,
                            Batch::const_iterator begin,
                            Batch::const_iterator end,
                            boost::optional<std::size_t> bytes) {
    if (begin == end) {
        return;
    }
    stdx::lock_guard<Latch> lk(_lastPushedMutex);
    _lastPushed = *(end - 1);
    _target->push(opCtx, begin, end, bytes);
}

void OplogBufferProxy::waitForSpace(OperationContext* opCtx, std::size_t size) {
    _target->waitForSpace(opCtx, size);
}

bool OplogBufferProxy::isEmpty() const {
    return _target->isEmpty();
}

std::size_t OplogBufferProxy::getMaxSize() const {
    return _target->getMaxSize();
}

std::size_t OplogBufferProxy::getSize() const {
    return _target->getSize();
}

std::size_t OplogBufferProxy::getCount() const {
    return _target->getCount();
}

void OplogBufferProxy::clear(OperationContext* opCtx) {
    stdx::lock_guard<Latch> backLock(_lastPushedMutex);
    stdx::lock_guard<Latch> frontLock(_lastPeekedMutex);
    _lastPushed.reset();
    _lastPeeked.reset();
    _target->clear(opCtx);
}

bool OplogBufferProxy::tryPop(OperationContext* opCtx, Value* value) {
    stdx::lock_guard<Latch> backLock(_lastPushedMutex);
    stdx::lock_guard<Latch> frontLock(_lastPeekedMutex);
    if (!_target->tryPop(opCtx, value)) {
        return false;
    }
    _lastPeeked.reset();
    // Reset _lastPushed if underlying buffer is empty.
    if (_target->isEmpty()) {
        _lastPushed.reset();
    }
    return true;
}

bool OplogBufferProxy::waitForDataFor(Milliseconds waitDuration, Interruptible* interruptible) {
    {
        stdx::unique_lock<Latch> lk(_lastPushedMutex);
        if (_lastPushed) {
            return true;
        }
    }
    return _target->waitForDataFor(waitDuration, interruptible);
}

bool OplogBufferProxy::waitForDataUntil(Date_t deadline, Interruptible* interruptible) {
    {
        stdx::unique_lock<Latch> lk(_lastPushedMutex);
        if (_lastPushed) {
            return true;
        }
    }
    return _target->waitForDataUntil(deadline, interruptible);
}

bool OplogBufferProxy::peek(OperationContext* opCtx, Value* value) {
    stdx::lock_guard<Latch> lk(_lastPeekedMutex);
    if (_lastPeeked) {
        *value = *_lastPeeked;
        return true;
    }
    if (_target->peek(opCtx, value)) {
        _lastPeeked = *value;
        return true;
    }
    return false;
}

boost::optional<OplogBuffer::Value> OplogBufferProxy::lastObjectPushed(
    OperationContext* opCtx) const {
    stdx::lock_guard<Latch> lk(_lastPushedMutex);
    if (!_lastPushed) {
        return boost::none;
    }
    return *_lastPushed;
}

boost::optional<OplogBuffer::Value> OplogBufferProxy::getLastPeeked_forTest() const {
    stdx::lock_guard<Latch> lk(_lastPeekedMutex);
    return _lastPeeked;
}

}  // namespace repl
}  // namespace mongo
