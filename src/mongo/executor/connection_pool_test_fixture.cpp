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

#include "mongo/platform/basic.h"

#include "mongo/executor/connection_pool_test_fixture.h"

#include <memory>


namespace mongo {
namespace executor {
namespace connection_pool_test_details {

TimerImpl::TimerImpl(PoolImpl* global) : _global(global) {}

TimerImpl::~TimerImpl() {
    cancelTimeout();
}

void TimerImpl::setTimeout(Milliseconds timeout, TimeoutCallback cb) {
    _cb = std::move(cb);
    _expiration = _global->now() + timeout;

    _timers.emplace(this);
}

void TimerImpl::cancelTimeout() {
    TimeoutCallback cb;
    _cb.swap(cb);

    _timers.erase(this);
}

void TimerImpl::clear() {
    while (!_timers.empty()) {
        auto* timer = *_timers.begin();
        timer->cancelTimeout();
    }
}

Date_t TimerImpl::now() {
    return _global->now();
}

void TimerImpl::fireIfNecessary() {
    auto timers = _timers;

    for (auto&& x : timers) {
        if (_timers.count(x) && (x->_expiration <= x->now())) {
            auto execCB = [cb = std::move(x->_cb)](auto&&) mutable { std::move(cb)(); };
            auto global = x->_global;
            _timers.erase(x);
            global->_executor->schedule(std::move(execCB));
        }
    }
}

std::set<TimerImpl*> TimerImpl::_timers;

ConnectionImpl::ConnectionImpl(const HostAndPort& hostAndPort, size_t generation, PoolImpl* global)
    : ConnectionInterface(generation),
      _hostAndPort(hostAndPort),
      _timer(global),
      _global(global),
      _id(_idCounter++) {}

Date_t ConnectionImpl::now() {
    return _timer.now();
}

size_t ConnectionImpl::id() const {
    return _id;
}

const HostAndPort& ConnectionImpl::getHostAndPort() const {
    return _hostAndPort;
}

bool ConnectionImpl::isHealthy() {
    return true;
}

void ConnectionImpl::clear() {
    _setupQueue.clear();
    _refreshQueue.clear();
    _pushSetupQueue.clear();
    _pushRefreshQueue.clear();
}

void ConnectionImpl::processSetup() {
    auto connPtr = _setupQueue.front();
    auto callback = std::move(_pushSetupQueue.front());
    _setupQueue.pop_front();
    _pushSetupQueue.pop_front();

    connPtr->_global->_executor->schedule([connPtr, callback = std::move(callback)](auto&&) {
        auto cb = std::move(connPtr->_setupCallback);
        connPtr->indicateUsed();
        cb(connPtr, callback());
    });
}

void ConnectionImpl::pushSetup(PushSetupCallback status) {
    _pushSetupQueue.push_back(std::move(status));

    if (_setupQueue.size()) {
        processSetup();
    }
}

void ConnectionImpl::pushSetup(Status status) {
    pushSetup([status]() { return status; });
}

size_t ConnectionImpl::setupQueueDepth() {
    return _setupQueue.size();
}

void ConnectionImpl::processRefresh() {
    auto connPtr = _refreshQueue.front();
    auto callback = std::move(_pushRefreshQueue.front());

    _refreshQueue.pop_front();
    _pushRefreshQueue.pop_front();

    connPtr->_global->_executor->schedule([connPtr, callback = std::move(callback)](auto&&) {
        auto cb = std::move(connPtr->_refreshCallback);
        connPtr->indicateUsed();
        cb(connPtr, callback());
    });
}

void ConnectionImpl::pushRefresh(PushRefreshCallback status) {
    _pushRefreshQueue.push_back(std::move(status));

    if (_refreshQueue.size()) {
        processRefresh();
    }
}

void ConnectionImpl::pushRefresh(Status status) {
    pushRefresh([status]() { return status; });
}

size_t ConnectionImpl::refreshQueueDepth() {
    return _refreshQueue.size();
}

void ConnectionImpl::setTimeout(Milliseconds timeout, TimeoutCallback cb) {
    _timer.setTimeout(timeout, std::move(cb));
}

void ConnectionImpl::cancelTimeout() {
    _timer.cancelTimeout();
}

void ConnectionImpl::setup(Milliseconds timeout, SetupCallback cb) {
    _setupCallback = std::move(cb);

    _timer.setTimeout(timeout, [this] {
        auto setupCb = std::move(_setupCallback);
        setupCb(this, Status(ErrorCodes::NetworkInterfaceExceededTimeLimit, "timeout"));
    });

    _setupQueue.push_back(this);

    if (_pushSetupQueue.size()) {
        processSetup();
    }
}

void ConnectionImpl::refresh(Milliseconds timeout, RefreshCallback cb) {
    _refreshCallback = std::move(cb);

    _timer.setTimeout(timeout, [this] {
        auto refreshCb = std::move(_refreshCallback);
        refreshCb(this, Status(ErrorCodes::NetworkInterfaceExceededTimeLimit, "timeout"));
    });

    _refreshQueue.push_back(this);

    if (_pushRefreshQueue.size()) {
        processRefresh();
    }
}

std::deque<ConnectionImpl::PushSetupCallback> ConnectionImpl::_pushSetupQueue;
std::deque<ConnectionImpl::PushRefreshCallback> ConnectionImpl::_pushRefreshQueue;
std::deque<ConnectionImpl*> ConnectionImpl::_setupQueue;
std::deque<ConnectionImpl*> ConnectionImpl::_refreshQueue;
size_t ConnectionImpl::_idCounter = 1;

std::shared_ptr<ConnectionPool::ConnectionInterface> PoolImpl::makeConnection(
    const HostAndPort& hostAndPort, transport::ConnectSSLMode sslMode, size_t generation) {
    return std::make_shared<ConnectionImpl>(hostAndPort, generation, this);
}

std::shared_ptr<ConnectionPool::TimerInterface> PoolImpl::makeTimer() {
    return std::make_unique<TimerImpl>(this);
}

const std::shared_ptr<OutOfLineExecutor>& PoolImpl::getExecutor() {
    return _executor;
}

Date_t PoolImpl::now() {
    return _now.get_value_or(Date_t::now());
}

void PoolImpl::setNow(Date_t now) {
    _now = now;
    TimerImpl::fireIfNecessary();
}

boost::optional<Date_t> PoolImpl::_now;

}  // namespace connection_pool_test_details
}  // namespace executor
}  // namespace mongo
