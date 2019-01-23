
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/default_baton.h"

#include "mongo/db/operation_context.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

DefaultBaton::DefaultBaton(OperationContext* opCtx) : _opCtx(opCtx) {}

DefaultBaton::~DefaultBaton() {
    invariant(!_opCtx);
    invariant(_scheduled.empty());
}

void DefaultBaton::markKillOnClientDisconnect() noexcept {
    if (_opCtx->getClient() && _opCtx->getClient()->session()) {
        _hasIngressSocket = true;
    }
}

void DefaultBaton::detachImpl() noexcept {
    decltype(_scheduled) scheduled;

    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);

        {
            stdx::lock_guard<Client> lk(*_opCtx->getClient());
            invariant(_opCtx->getBaton().get() == this);
            _opCtx->setBaton(nullptr);
        }

        _opCtx = nullptr;
        _hasIngressSocket = false;

        using std::swap;
        swap(_scheduled, scheduled);
    }

    for (auto& job : scheduled) {
        job(nullptr);
    }
}

void DefaultBaton::schedule(unique_function<void(OperationContext*)> func) noexcept {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    if (!_opCtx) {
        lk.unlock();
        func(nullptr);

        return;
    }

    _scheduled.push_back(std::move(func));

    if (_sleeping && !_notified) {
        _notified = true;
        _cv.notify_one();
    }
}

void DefaultBaton::notify() noexcept {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _notified = true;
    _cv.notify_one();
}

Waitable::TimeoutState DefaultBaton::run_until(ClockSource* clkSource,
                                               Date_t oldDeadline) noexcept {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    // We'll fulfill promises and run jobs on the way out, ensuring we don't hold any locks
    const auto guard = makeGuard([&] {
        // While we have scheduled work, keep running jobs
        while (_scheduled.size()) {
            auto toRun = std::exchange(_scheduled, {});

            lk.unlock();
            for (auto& job : toRun) {
                job(_opCtx);
            }
            lk.lock();
        }
    });

    // If anything was scheduled, run it now.
    if (_scheduled.size()) {
        return Waitable::TimeoutState::NoTimeout;
    }

    auto newDeadline = oldDeadline;

    // If we have an ingress socket, sleep no more than 1 second (so we poll for closure in the
    // outside opCtx waitForConditionOrInterruptUntil implementation)
    if (_hasIngressSocket) {
        newDeadline = std::min(oldDeadline, clkSource->now() + Seconds(1));
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

}  // namespace mongo
