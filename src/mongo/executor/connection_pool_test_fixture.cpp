// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/executor/connection_pool_test_fixture.h"

#include "mongo/base/error_codes.h"
#include "mongo/executor/connection_pool.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>


namespace mongo {
namespace executor {
namespace connection_pool_test_details {

TimerImpl::TimerImpl(PoolImpl* global) : _global(global) {}

TimerImpl::~TimerImpl() {
    cancelTimeout();
}

void TimerImpl::setTimeout(Milliseconds timeout, TimeoutCallback cb) {
    _cb = std::move(cb);
    invariant(timeout >= Milliseconds(0));
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
            auto execCB = [cb = std::move(x->_cb)](auto&&) mutable {
                std::move(cb)();
            };
            auto global = x->_global;
            _timers.erase(x);
            global->_executor->schedule(std::move(execCB));
        }
    }
}

std::set<TimerImpl*> TimerImpl::_timers;

ConnectionImpl::ConnectionImpl(const HostAndPort& hostAndPort,
                               PoolConnectionId id,
                               size_t generation,
                               PoolImpl* global)
    : ConnectionInterface(id, generation),
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
    return _healthy;
}

void ConnectionImpl::setUnhealthy() {
    _healthy = false;
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

void ConnectionImpl::setup(Milliseconds timeout, SetupCallback cb, std::string) {
    _setupCallback = std::move(cb);

    _timer.setTimeout(timeout, [this, timeout] {
        auto setupCb = std::move(_setupCallback);
        std::string reason = str::stream()
            << "Timed out connecting to " << _hostAndPort << " after " << timeout;
        setupCb(this, Status(ErrorCodes::ConnectionEstablishmentTimeout, std::move(reason)));
    });

    _setupQueue.push_back(this);

    if (_pushSetupQueue.size()) {
        processSetup();
    }
}

void ConnectionImpl::refresh(Milliseconds timeout, RefreshCallback cb) {
    _refreshCallback = std::move(cb);

    _timer.setTimeout(timeout, [this, timeout] {
        auto refreshCb = std::move(_refreshCallback);
        std::string reason = str::stream()
            << "Timed out refreshing host " << _hostAndPort << " after " << timeout;
        refreshCb(this, Status(ErrorCodes::HostUnreachable, std::move(reason)));
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
    const HostAndPort& hostAndPort,
    transport::ConnectSSLMode sslMode,
    PoolConnectionId id,
    size_t generation) {
    return std::make_shared<ConnectionImpl>(hostAndPort, id, generation, this);
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
    if (_now) {
        // If we're not initializing the virtual clock, advance the fast clock source as well.
        Milliseconds diff = now - *_now;
        _fastClockSource.advance(diff);
    }
    _now = now;
    TimerImpl::fireIfNecessary();
}

boost::optional<Date_t> PoolImpl::_now;
ClockSourceMock PoolImpl::_fastClockSource;

}  // namespace connection_pool_test_details
}  // namespace executor
}  // namespace mongo
