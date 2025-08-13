/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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


#include "mongo/db/default_baton.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/functional.h"
#include "mongo/util/future.h"

#include <algorithm>
#include <mutex>
#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {

namespace {

const auto kDetached = Status(ErrorCodes::ShutdownInProgress, "Baton detached");

}  // namespace

DefaultBaton::DefaultBaton(OperationContext* opCtx) : _opCtx(opCtx) {}

DefaultBaton::~DefaultBaton() {
    invariant(!_opCtx);
    invariant(_scheduled.empty());
}

void DefaultBaton::detachImpl() {
    decltype(_scheduled) scheduled;
    decltype(_timers) timers;

    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);

        invariant(_opCtx->getBaton().get() == this);
        _opCtx->setBaton(nullptr);

        _opCtx = nullptr;

        using std::swap;
        swap(_scheduled, scheduled);
        swap(_timers, timers);
        _timersById.clear();
    }

    for (auto& timer : timers) {
        timer.second.promise.setError(kDetached);
    }

    for (auto& job : scheduled) {
        job(kDetached);
    }
}

void DefaultBaton::schedule(Task func) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    if (!_opCtx) {
        lk.unlock();
        func(kDetached);

        return;
    }

    _scheduled.push_back(std::move(func));

    if (_sleeping && !_notified) {
        _notified = true;
        _cv.notify_one();
    }
}

void DefaultBaton::_notify(stdx::unique_lock<stdx::mutex> lk) noexcept {
    dassert(lk.mutex() == &_mutex);

    _notified = true;
    _cv.notify_one();
}

void DefaultBaton::notify() noexcept {
    _notify(stdx::unique_lock<stdx::mutex>(_mutex));
}

Waitable::TimeoutState DefaultBaton::run_until(ClockSource* clkSource,
                                               Date_t oldDeadline) noexcept {
    // We'll fulfill promises and run jobs on the way out, ensuring we don't hold any locks
    const ScopeGuard guard([&] {
        stdx::unique_lock<stdx::mutex> lk(_mutex);

        // Fire expired timers
        std::vector<Timer> expiredTimers;
        const auto now = clkSource->now();
        for (auto it = _timers.begin(); it != _timers.end() && it->first <= now;
             it = _timers.erase(it)) {
            _timersById.erase(it->second.id);
            expiredTimers.push_back(std::move(it->second));
        }

        lk.unlock();
        for (auto& timer : expiredTimers) {
            timer.promise.emplaceValue();
        }
        lk.lock();

        // While we have scheduled work, keep running jobs
        while (_scheduled.size()) {
            auto toRun = std::exchange(_scheduled, {});

            lk.unlock();
            for (auto& job : toRun) {
                job(Status::OK());
            }
            lk.lock();
        }
    });

    stdx::unique_lock<stdx::mutex> lk(_mutex);

    // If anything was scheduled, run it now.
    if (_scheduled.size()) {
        return Waitable::TimeoutState::NoTimeout;
    }

    auto newDeadline = oldDeadline;

    if (!_timers.empty()) {
        newDeadline = std::min(oldDeadline, _timers.begin()->first);
    }

    // we mark sleeping, so that we receive notifications
    _sleeping = true;

    // while we're not notified
    auto notified =
        clkSource->waitForConditionUntil(_cv, lk, newDeadline, [&] { return _notified; });

    _sleeping = false;
    _notified = false;

    // If we've been notified, or we haven't timed out yet, return as if by spurious wakeup.
    // Otherwise call it a timeout.
    return (notified || (clkSource->now() < oldDeadline)) ? Waitable::TimeoutState::NoTimeout
                                                          : Waitable::TimeoutState::Timeout;
}

void DefaultBaton::run(ClockSource* clkSource) noexcept {
    run_until(clkSource, Date_t::max());
}

void DefaultBaton::_safeExecute(stdx::unique_lock<stdx::mutex> lk, Job job) {
    dassert(lk.mutex() == &_mutex);

    if (!_opCtx) {
        // If we're detached, no job can safely execute.
        iasserted(kDetached);
    }

    if (_sleeping) {
        _scheduled.push_back([this, job = std::move(job)](auto status) mutable {
            if (status.isOK()) {
                job(stdx::unique_lock<stdx::mutex>(_mutex));
            }
        });
        _notify(std::move(lk));
    } else {
        job(std::move(lk));
    }
}

Future<void> DefaultBaton::waitUntil(Date_t expiration, const CancellationToken& token) try {
    auto pf = makePromiseFuture<void>();
    auto id = _nextTimerId.fetchAndAdd(1);
    _safeExecute(stdx::unique_lock(_mutex),
                 [this, id, expiration, promise = std::move(pf.promise)](auto lk) mutable {
                     auto iter = _timers.emplace(expiration, Timer{id, std::move(promise)});
                     _timersById[iter->second.id] = iter;
                 });

    token.onCancel().thenRunOn(shared_from_this()).getAsync([this, id](Status s) {
        if (s.isOK()) {
            stdx::unique_lock lk(_mutex);
            if (_timersById.find(id) == _timersById.end()) {
                return;
            }

            _safeExecute(std::move(lk), [this, id](auto lk) {
                auto it = _timersById.find(id);
                if (it == _timersById.end()) {
                    return;
                }
                auto timer = std::exchange(it->second->second, {});
                _timers.erase(it->second);
                _timersById.erase(it);
                lk.unlock();

                timer.promise.setError(
                    Status(ErrorCodes::CallbackCanceled, "Baton wait cancelled"));
            });
        }
    });
    return std::move(pf.future);
} catch (const DBException& e) {
    return e.toStatus();
}

}  // namespace mongo
