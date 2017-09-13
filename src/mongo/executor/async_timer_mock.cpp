/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/executor/async_timer_mock.h"

#include "mongo/stdx/memory.h"

namespace mongo {
namespace executor {

namespace {
const Milliseconds kZeroMilliseconds = Milliseconds(0);
}  // namespace

AsyncTimerMockImpl::AsyncTimerMockImpl(Milliseconds expiration) : _timeLeft(expiration) {}

void AsyncTimerMockImpl::cancel() {
    _callAllHandlers(asio::error::operation_aborted);
}

void AsyncTimerMockImpl::asyncWait(AsyncTimerInterface::Handler handler) {
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        if (_timeLeft != kZeroMilliseconds) {
            _handlers.push_back(handler);
            return;
        }
    }

    // If we have expired, run handler now instead of storing.
    // Callbacks scheduled for now will fire immediately, synchronously.
    handler(std::error_code());
}

void AsyncTimerMockImpl::fastForward(Milliseconds time) {
    std::vector<AsyncTimerInterface::Handler> tmp;

    // While holding the lock, change the time and remove
    // handlers that have expired
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        if (time >= _timeLeft) {
            _timeLeft = kZeroMilliseconds;
            tmp.swap(_handlers);
        } else {
            _timeLeft -= time;
        }
    }

    // If handlers expired, call them outside of the lock
    for (const auto& handler : tmp) {
        handler(std::error_code());
    }
}

Milliseconds AsyncTimerMockImpl::timeLeft() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _timeLeft;
}

void AsyncTimerMockImpl::expireAfter(Milliseconds expiration) {
    std::vector<AsyncTimerInterface::Handler> tmp;

    // While holding the lock, reset the time and remove all handlers
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _timeLeft = expiration;
        tmp.swap(_handlers);
    }

    // Call handlers with a "canceled" error code
    for (const auto& handler : tmp) {
        handler(asio::error::operation_aborted);
    }
}

int AsyncTimerMockImpl::jobs() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _handlers.size();
}

void AsyncTimerMockImpl::_callAllHandlers(std::error_code ec) {
    std::vector<AsyncTimerInterface::Handler> tmp;
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        tmp.swap(_handlers);
    }

    for (const auto& handler : tmp) {
        handler(ec);
    }
}

AsyncTimerMock::AsyncTimerMock(std::shared_ptr<AsyncTimerMockImpl> timer) : _timer(timer) {}

void AsyncTimerMock::cancel() {
    _timer->cancel();
}

void AsyncTimerMock::asyncWait(AsyncTimerInterface::Handler handler) {
    _timer->asyncWait(handler);
}

void AsyncTimerMock::expireAfter(Milliseconds expiration) {
    _timer->expireAfter(expiration);
}

std::unique_ptr<AsyncTimerInterface> AsyncTimerFactoryMock::make(Milliseconds expiration) {
    return make(nullptr, expiration);
}

std::unique_ptr<AsyncTimerInterface> AsyncTimerFactoryMock::make(asio::io_service::strand* strand,
                                                                 Milliseconds expiration) {
    stdx::lock_guard<stdx::recursive_mutex> lk(_timersMutex);
    auto elem = _timers.emplace(std::make_shared<AsyncTimerMockImpl>(expiration));
    return stdx::make_unique<AsyncTimerMock>(*elem.first);
}

void AsyncTimerFactoryMock::fastForward(Milliseconds time) {
    stdx::lock_guard<stdx::recursive_mutex> lk(_timersMutex);

    _curTime += time;

    // Timers may be reset, so keep them in our set even if they have expired.
    for (auto elem = _timers.begin(); elem != _timers.end(); elem++) {
        auto timer = *elem;
        timer->fastForward(time);
    }
}

Date_t AsyncTimerFactoryMock::now() {
    stdx::lock_guard<stdx::recursive_mutex> lk(_timersMutex);
    return Date_t::fromDurationSinceEpoch(_curTime);
}

int AsyncTimerFactoryMock::jobs() {
    int jobs = 1;

    stdx::lock_guard<stdx::recursive_mutex> lk(_timersMutex);
    for (auto elem = _timers.begin(); elem != _timers.end(); elem++) {
        jobs += (*elem)->jobs();
    }

    return jobs;
}

}  // namespace executor
}  // namespace mongo
