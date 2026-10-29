// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/oplog_buffer_proxy.h"

#include "mongo/util/assert_util.h"

#include <mutex>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

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
        std::lock_guard<std::mutex> backLock(_lastPushedMutex);
        std::lock_guard<std::mutex> frontLock(_lastPeekedMutex);
        _lastPushed.reset();
        _lastPeeked.reset();
    }
    _target->shutdown(opCtx);
}

void OplogBufferProxy::push(OperationContext* opCtx,
                            Batch::const_iterator begin,
                            Batch::const_iterator end,
                            boost::optional<const Cost&> cost) {
    if (begin == end) {
        return;
    }
    std::lock_guard<std::mutex> lk(_lastPushedMutex);
    _lastPushed = *(end - 1);
    _target->push(opCtx, begin, end, cost);
}

void OplogBufferProxy::waitForSpace(OperationContext* opCtx, const Cost& cost) {
    _target->waitForSpace(opCtx, cost);
}

bool OplogBufferProxy::isEmpty() const {
    return _target->isEmpty();
}

std::size_t OplogBufferProxy::getSize() const {
    return _target->getSize();
}

std::size_t OplogBufferProxy::getCount() const {
    return _target->getCount();
}

void OplogBufferProxy::clear(OperationContext* opCtx) {
    std::lock_guard<std::mutex> backLock(_lastPushedMutex);
    std::lock_guard<std::mutex> frontLock(_lastPeekedMutex);
    _lastPushed.reset();
    _lastPeeked.reset();
    _target->clear(opCtx);
}

bool OplogBufferProxy::tryPop(OperationContext* opCtx, Value* value) {
    std::lock_guard<std::mutex> backLock(_lastPushedMutex);
    std::lock_guard<std::mutex> frontLock(_lastPeekedMutex);
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
        std::unique_lock<std::mutex> lk(_lastPushedMutex);
        if (_lastPushed) {
            return true;
        }
    }
    return _target->waitForDataFor(waitDuration, interruptible);
}

bool OplogBufferProxy::waitForDataUntil(Date_t deadline, Interruptible* interruptible) {
    {
        std::unique_lock<std::mutex> lk(_lastPushedMutex);
        if (_lastPushed) {
            return true;
        }
    }
    return _target->waitForDataUntil(deadline, interruptible);
}

bool OplogBufferProxy::peek(OperationContext* opCtx, Value* value) {
    std::lock_guard<std::mutex> lk(_lastPeekedMutex);
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
    std::lock_guard<std::mutex> lk(_lastPushedMutex);
    if (!_lastPushed) {
        return boost::none;
    }
    return *_lastPushed;
}

boost::optional<OplogBuffer::Value> OplogBufferProxy::getLastPeeked_forTest() const {
    std::lock_guard<std::mutex> lk(_lastPeekedMutex);
    return _lastPeeked;
}

}  // namespace repl
}  // namespace mongo
