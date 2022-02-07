/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

#include "mongo/transport/baton_asio_linux.h"

#include "mongo/base/checked_cast.h"
#include "mongo/db/operation_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace transport {

const Client::Decoration<TransportLayerASIO::BatonASIO::EventFDHolder>
    TransportLayerASIO::BatonASIO::EventFDHolder::getForClient =
        Client::declareDecoration<TransportLayerASIO::BatonASIO::EventFDHolder>();

namespace {

MONGO_FAIL_POINT_DEFINE(blockBatonASIOBeforePoll);

Status getDetachedError() {
    return {ErrorCodes::ShutdownInProgress, "Baton detached"};
}

Status getCanceledError() {
    return {ErrorCodes::CallbackCanceled, "Baton wait canceled"};
}

/**
 * We use this internal reactor timer to exit run_until calls (by forcing an early timeout for
 * ::poll).
 *
 * Its methods are all unreachable because we never actually use its timer-ness (we just need its
 * address for baton book keeping).
 */
class InternalReactorTimer : public ReactorTimer {
public:
    void cancel(const BatonHandle& baton = nullptr) override {
        MONGO_UNREACHABLE;
    }

    Future<void> waitUntil(Date_t timeout, const BatonHandle& baton = nullptr) override {
        MONGO_UNREACHABLE;
    }
};

}  // namespace

void TransportLayerASIO::BatonASIO::schedule(Task func) noexcept {
    stdx::unique_lock lk(_mutex);

    if (!_opCtx) {
        lk.unlock();
        func(getDetachedError());

        return;
    }

    _scheduled.push_back(
        [ this, func = std::move(func) ](stdx::unique_lock<Mutex> lk) mutable noexcept {
            auto status = Status::OK();
            if (!_opCtx) {
                status = getDetachedError();
            }
            lk.unlock();

            func(status);
        });

    if (_inPoll) {
        _efd().notify();
    }
}

void TransportLayerASIO::BatonASIO::notify() noexcept {
    _efd().notify();
}

/**
 * We synthesize a run_until by creating a synthetic timer which we use to exit run early (we create
 * a regular waitUntil baton event off the timer, with the passed deadline).
 */
Waitable::TimeoutState TransportLayerASIO::BatonASIO::run_until(ClockSource* clkSource,
                                                                Date_t deadline) noexcept {
    InternalReactorTimer irt;
    auto future = waitUntil(irt, deadline);

    run(clkSource);

    // If the future is ready our timer has fired, in which case we timed out
    if (future.isReady()) {
        future.get();

        return Waitable::TimeoutState::Timeout;
    } else {
        cancelTimer(irt);

        return Waitable::TimeoutState::NoTimeout;
    }
}

void TransportLayerASIO::BatonASIO::run(ClockSource* clkSource) noexcept {
    std::vector<Promise<void>> toFulfill;

    // We'll fulfill promises and run jobs on the way out, ensuring we don't hold any locks
    const ScopeGuard guard([&] {
        for (auto& promise : toFulfill) {
            promise.emplaceValue();
        }

        auto lk = stdx::unique_lock(_mutex);
        while (_scheduled.size()) {
            auto scheduled = std::exchange(_scheduled, {});
            for (auto& job : scheduled) {
                job(std::move(lk));
                job = nullptr;

                lk = stdx::unique_lock(_mutex);
            }
        }
    });

    stdx::unique_lock lk(_mutex);

    // If anything was scheduled, run it now.  No need to poll
    if (_scheduled.size()) {
        return;
    }

    boost::optional<Date_t> deadline;

    // If we have a timer, poll no longer than that
    if (_timers.size()) {
        deadline = _timers.begin()->first;
    }

    _pollSessions.clear();
    _pollSet.clear();
    _pollSessions.reserve(_sessions.size());
    _pollSet.reserve(_sessions.size() + 1);

    _pollSet.push_back(pollfd{_efd().fd, POLLIN, 0});

    for (auto iter = _sessions.begin(); iter != _sessions.end(); ++iter) {
        _pollSet.push_back(pollfd{iter->second.fd, iter->second.type, 0});
        _pollSessions.push_back(iter);
    }

    auto now = clkSource->now();

    int rval = 0;
    // If we don't have a timeout, or we have a timeout that's unexpired, run poll.
    if (!deadline || (*deadline > now)) {
        if (deadline && !clkSource->tracksSystemClock()) {
            invariant(
                clkSource->setAlarm(*deadline, [this, anchor = shared_from_this()] { notify(); }));

            deadline.reset();
        }

        _inPoll = true;
        lk.unlock();
        blockBatonASIOBeforePoll.pauseWhileSet();
        rval = ::poll(_pollSet.data(),
                      _pollSet.size(),
                      deadline ? Milliseconds(*deadline - now).count() : -1);
        auto savedErrno = errno;
        lk.lock();
        _inPoll = false;

        // If poll failed, it better be in EINTR
        if (rval < 0 && savedErrno != EINTR) {
            LOGV2_FATAL(50834,
                        "error in poll: {error}",
                        "error in poll",
                        "error"_attr = errnoWithDescription(savedErrno));
        }
    }

    now = clkSource->now();

    // Fire expired timers
    for (auto iter = _timers.begin(); iter != _timers.end() && iter->first <= now;) {
        toFulfill.push_back(std::move(iter->second.promise));
        _timersById.erase(iter->second.id);
        iter = _timers.erase(iter);
    }

    // If poll found some activity
    if (rval > 0) {
        size_t remaining = rval;

        auto pollIter = _pollSet.begin();

        if (pollIter->revents) {
            _efd().wait();

            remaining--;
        }

        ++pollIter;

        for (auto sessionIter = _pollSessions.begin();
             sessionIter != _pollSessions.end() && remaining;
             ++sessionIter, ++pollIter) {
            if (pollIter->revents) {
                toFulfill.push_back(std::move((*sessionIter)->second.promise));
                _sessions.erase(*sessionIter);

                remaining--;
            }
        }

        invariant(remaining == 0);
    }

    return;
}

void TransportLayerASIO::BatonASIO::markKillOnClientDisconnect() noexcept {
    if (_opCtx->getClient() && _opCtx->getClient()->session()) {
        _addSession(*(_opCtx->getClient()->session()), POLLRDHUP).getAsync([this](Status s) {
            if (!s.isOK()) {
                return;
            }

            _opCtx->markKilled(ErrorCodes::ClientDisconnect);
        });
    }
}

Future<void> TransportLayerASIO::BatonASIO::addSession(Session& session, Type type) noexcept {
    return _addSession(session, type == Type::In ? POLLIN : POLLOUT);
}

Future<void> TransportLayerASIO::BatonASIO::waitUntil(const ReactorTimer& timer,
                                                      Date_t expiration) noexcept try {
    auto pf = makePromiseFuture<void>();
    auto id = timer.id();

    stdx::unique_lock lk(_mutex);

    _safeExecute(std::move(lk), [ expiration, timer = Timer{id, std::move(pf.promise)},
                                  this ](stdx::unique_lock<Mutex>) mutable noexcept {
        auto iter = _timers.emplace(expiration, std::move(timer));
        _timersById[iter->second.id] = iter;
    });

    return std::move(pf.future);
} catch (const DBException& ex) {
    return ex.toStatus();
}

bool TransportLayerASIO::BatonASIO::cancelSession(Session& session) noexcept {
    const auto id = session.id();

    stdx::unique_lock lk(_mutex);

    if (_sessions.find(id) == _sessions.end()) {
        return false;
    }

    _safeExecute(std::move(lk), [ id, this ](stdx::unique_lock<Mutex> lk) noexcept {
        auto iter = _sessions.find(id);
        if (iter == _sessions.end()) {
            return;
        }
        auto session = std::exchange(iter->second, {});
        _sessions.erase(iter);
        lk.unlock();

        session.promise.setError(getCanceledError());
    });

    return true;
}

bool TransportLayerASIO::BatonASIO::cancelTimer(const ReactorTimer& timer) noexcept {
    const auto id = timer.id();

    stdx::unique_lock lk(_mutex);

    if (_timersById.find(id) == _timersById.end()) {
        return false;
    }

    _safeExecute(std::move(lk), [ id, this ](stdx::unique_lock<Mutex> lk) noexcept {
        auto iter = _timersById.find(id);

        if (iter == _timersById.end()) {
            return;
        }

        auto timer = std::exchange(iter->second->second, {});
        _timers.erase(iter->second);
        _timersById.erase(iter);
        lk.unlock();

        timer.promise.setError(getCanceledError());
    });

    return true;
}

bool TransportLayerASIO::BatonASIO::canWait() noexcept {
    stdx::lock_guard lk(_mutex);
    return _opCtx;
}

TransportLayerASIO::BatonASIO::EventFDHolder& TransportLayerASIO::BatonASIO::_efd() {
    return EventFDHolder::getForClient(_opCtx->getClient());
}

Future<void> TransportLayerASIO::BatonASIO::_addSession(Session& session, short type) noexcept try {
    auto fd = checked_cast<ASIOSession&>(session).getSocket().native_handle();
    auto id = session.id();
    auto pf = makePromiseFuture<void>();

    stdx::unique_lock lk(_mutex);

    _safeExecute(std::move(lk),
                 [ id, session = TransportSession{fd, type, std::move(pf.promise)},
                   this ](stdx::unique_lock<Mutex>) mutable noexcept {
                     auto ret = _sessions.emplace(id, std::move(session));
                     invariant(ret.second);
                 });
    return std::move(pf.future);
} catch (const DBException& ex) {
    return ex.toStatus();
}

void TransportLayerASIO::BatonASIO::detachImpl() noexcept {
    decltype(_scheduled) scheduled;
    decltype(_sessions) sessions;
    decltype(_timers) timers;

    {
        stdx::lock_guard lk(_mutex);

        invariant(_opCtx->getBaton().get() == this);
        _opCtx->setBaton(nullptr);

        _opCtx = nullptr;

        using std::swap;
        swap(_scheduled, scheduled);
        swap(_sessions, sessions);
        swap(_timers, timers);
    }

    for (auto& job : scheduled) {
        job(stdx::unique_lock(_mutex));
        job = nullptr;
    }

    for (auto& session : sessions) {
        session.second.promise.setError(getDetachedError());
    }

    for (auto& pair : timers) {
        pair.second.promise.setError(getDetachedError());
    }
}

}  // namespace transport
}  // namespace mongo
