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
    // If we have expired, run handler now instead of storing.
    if (_timeLeft == kZeroMilliseconds) {
        // Callbacks scheduled for now will fire immediately, synchronously.
        handler(std::error_code());
    } else {
        _handlers.push_back(handler);
    }
}

bool AsyncTimerMockImpl::fastForward(Milliseconds time) {
    if (time >= _timeLeft) {
        _timeLeft = kZeroMilliseconds;
        _expire();
    } else {
        _timeLeft -= time;
    }

    return _timeLeft > kZeroMilliseconds;
}

Milliseconds AsyncTimerMockImpl::timeLeft() {
    return _timeLeft;
}

void AsyncTimerMockImpl::_callAllHandlers(std::error_code ec) {
    for (auto elem = _handlers.begin(); elem != _handlers.end(); elem++) {
        const auto& handler = *elem;
        handler(ec);
    }
    _handlers.clear();
}

void AsyncTimerMockImpl::_expire() {
    _callAllHandlers(std::error_code());
}

AsyncTimerMock::AsyncTimerMock(std::shared_ptr<AsyncTimerMockImpl> timer) : _timer(timer) {}

void AsyncTimerMock::cancel() {
    _timer->cancel();
}

void AsyncTimerMock::asyncWait(AsyncTimerInterface::Handler handler) {
    _timer->asyncWait(handler);
}

std::unique_ptr<AsyncTimerInterface> AsyncTimerFactoryMock::make(Milliseconds expiration) {
    return make(nullptr, expiration);
}

std::unique_ptr<AsyncTimerInterface> AsyncTimerFactoryMock::make(asio::io_service::strand* strand,
                                                                 Milliseconds expiration) {
    stdx::lock_guard<stdx::mutex> lk(_timersMutex);
    auto elem = _timers.emplace(std::make_shared<AsyncTimerMockImpl>(expiration));
    return stdx::make_unique<AsyncTimerMock>(*elem.first);
}

void AsyncTimerFactoryMock::fastForward(Milliseconds time) {
    stdx::lock_guard<stdx::mutex> lk(_timersMutex);

    // erase after iterating to be safe
    std::unordered_set<std::shared_ptr<AsyncTimerMockImpl>> expired;
    for (auto elem = _timers.begin(); elem != _timers.end(); elem++) {
        auto timer = *elem;

        // If timer has expired, register it for removal from our set.
        if (!timer->fastForward(time)) {
            expired.insert(timer);
        }
    }

    for (auto elem = expired.begin(); elem != expired.end(); elem++) {
        _timers.erase(*elem);
    }
}

}  // namespace executor
}  // namespace mongo
