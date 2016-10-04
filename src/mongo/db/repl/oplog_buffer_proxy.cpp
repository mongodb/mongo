/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */
#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplication

#include "mongo/platform/basic.h"

#include "mongo/db/repl/oplog_buffer_proxy.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace repl {

OplogBufferProxy::OplogBufferProxy(std::unique_ptr<OplogBuffer> target)
    : _target(std::move(target)) {
    invariant(_target);
}

OplogBuffer* OplogBufferProxy::getTarget() const {
    return _target.get();
}

void OplogBufferProxy::startup(OperationContext* txn) {
    _target->startup(txn);
}

void OplogBufferProxy::shutdown(OperationContext* txn) {
    {
        stdx::lock_guard<stdx::mutex> backLock(_lastPushedMutex);
        stdx::lock_guard<stdx::mutex> frontLock(_lastPeekedMutex);
        _lastPushed.reset();
        _lastPeeked.reset();
    }
    _target->shutdown(txn);
}

void OplogBufferProxy::pushEvenIfFull(OperationContext* txn, const Value& value) {
    stdx::lock_guard<stdx::mutex> lk(_lastPushedMutex);
    _lastPushed = value;
    _target->pushEvenIfFull(txn, value);
}

void OplogBufferProxy::push(OperationContext* txn, const Value& value) {
    stdx::lock_guard<stdx::mutex> lk(_lastPushedMutex);
    _lastPushed = value;
    _target->push(txn, value);
}

void OplogBufferProxy::pushAllNonBlocking(OperationContext* txn,
                                          Batch::const_iterator begin,
                                          Batch::const_iterator end) {
    if (begin == end) {
        return;
    }
    stdx::lock_guard<stdx::mutex> lk(_lastPushedMutex);
    _lastPushed = *(end - 1);
    _target->pushAllNonBlocking(txn, begin, end);
}

void OplogBufferProxy::waitForSpace(OperationContext* txn, std::size_t size) {
    _target->waitForSpace(txn, size);
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

void OplogBufferProxy::clear(OperationContext* txn) {
    stdx::lock_guard<stdx::mutex> backLock(_lastPushedMutex);
    stdx::lock_guard<stdx::mutex> frontLock(_lastPeekedMutex);
    _lastPushed.reset();
    _lastPeeked.reset();
    _target->clear(txn);
}

bool OplogBufferProxy::tryPop(OperationContext* txn, Value* value) {
    stdx::lock_guard<stdx::mutex> backLock(_lastPushedMutex);
    stdx::lock_guard<stdx::mutex> frontLock(_lastPeekedMutex);
    if (!_target->tryPop(txn, value)) {
        return false;
    }
    _lastPeeked.reset();
    // Reset _lastPushed if underlying buffer is empty.
    if (_target->isEmpty()) {
        _lastPushed.reset();
    }
    return true;
}

bool OplogBufferProxy::waitForData(Seconds waitDuration) {
    {
        stdx::unique_lock<stdx::mutex> lk(_lastPushedMutex);
        if (_lastPushed) {
            return true;
        }
    }
    return _target->waitForData(waitDuration);
}

bool OplogBufferProxy::peek(OperationContext* txn, Value* value) {
    stdx::lock_guard<stdx::mutex> lk(_lastPeekedMutex);
    if (_lastPeeked) {
        *value = *_lastPeeked;
        return true;
    }
    if (_target->peek(txn, value)) {
        _lastPeeked = *value;
        return true;
    }
    return false;
}

boost::optional<OplogBuffer::Value> OplogBufferProxy::lastObjectPushed(
    OperationContext* txn) const {
    stdx::lock_guard<stdx::mutex> lk(_lastPushedMutex);
    if (!_lastPushed) {
        return boost::none;
    }
    return *_lastPushed;
}

boost::optional<OplogBuffer::Value> OplogBufferProxy::getLastPeeked_forTest() const {
    stdx::lock_guard<stdx::mutex> lk(_lastPeekedMutex);
    return _lastPeeked;
}

}  // namespace repl
}  // namespace mongo
