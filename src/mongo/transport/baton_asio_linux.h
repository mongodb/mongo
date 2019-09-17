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

#pragma once

#include <map>
#include <memory>
#include <vector>

#include <poll.h>
#include <sys/eventfd.h>

#include "mongo/base/checked_cast.h"
#include "mongo/db/operation_context.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/transport/baton.h"
#include "mongo/transport/session_asio.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/future.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace transport {

/**
 * TransportLayerASIO Baton implementation for linux.
 *
 * We implement our networking reactor on top of poll + eventfd for wakeups
 */
class TransportLayerASIO::BatonASIO : public NetworkingBaton {
    static const inline auto kDetached = Status(ErrorCodes::ShutdownInProgress, "Baton detached");

    /**
     * We use this internal reactor timer to exit run_until calls (by forcing an early timeout for
     * ::poll).
     *
     * Its methods are all unreachable because we never actually use its timer-ness (we just need
     * its address for baton book keeping).
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

    /**
     * RAII type that wraps up an eventfd and reading/writing to it.  We don't actually need the
     * counter portion, just the notify/wakeup
     */
    struct EventFDHolder {
        EventFDHolder() : fd(::eventfd(0, EFD_CLOEXEC)) {
            if (fd < 0) {
                auto e = errno;
                std::string reason = str::stream()
                    << "error in creating eventfd: " << errnoWithDescription(e);

                auto code = (e == EMFILE || e == ENFILE) ? ErrorCodes::TooManyFilesOpen
                                                         : ErrorCodes::UnknownError;

                uasserted(code, reason);
            }
        }

        ~EventFDHolder() {
            ::close(fd);
        }

        EventFDHolder(const EventFDHolder&) = delete;
        EventFDHolder& operator=(const EventFDHolder&) = delete;

        // Writes to the underlying eventfd
        void notify() {
            while (true) {
                if (::eventfd_write(fd, 1) == 0) {
                    break;
                }

                invariant(errno == EINTR);
            }
        }

        void wait() {
            while (true) {
                // If we have activity on the eventfd, pull the count out
                uint64_t u;
                if (::eventfd_read(fd, &u) == 0) {
                    break;
                }

                invariant(errno == EINTR);
            }
        }

        const int fd;

        static const Client::Decoration<EventFDHolder> getForClient;
    };

public:
    BatonASIO(OperationContext* opCtx) : _opCtx(opCtx) {}

    ~BatonASIO() {
        invariant(!_opCtx);
        invariant(_sessions.empty());
        invariant(_scheduled.empty());
        invariant(_timers.empty());
    }

    void markKillOnClientDisconnect() noexcept override {
        if (_opCtx->getClient() && _opCtx->getClient()->session()) {
            addSessionImpl(*(_opCtx->getClient()->session()), POLLRDHUP).getAsync([this](Status s) {
                if (!s.isOK()) {
                    return;
                }

                _opCtx->markKilled(ErrorCodes::ClientDisconnect);
            });
        }
    }

    Future<void> addSession(Session& session, Type type) noexcept override {
        return addSessionImpl(session, type == Type::In ? POLLIN : POLLOUT);
    }

    Future<void> waitUntil(const ReactorTimer& timer, Date_t expiration) noexcept override {
        auto pf = makePromiseFuture<void>();
        auto id = timer.id();

        stdx::unique_lock<Latch> lk(_mutex);

        if (!_opCtx) {
            return kDetached;
        }

        _safeExecute(std::move(lk),
                     [id, expiration, promise = std::move(pf.promise), this]() mutable {
                         auto iter = _timers.emplace(std::piecewise_construct,
                                                     std::forward_as_tuple(expiration),
                                                     std::forward_as_tuple(id, std::move(promise)));
                         _timersById[id] = iter;
                     });

        return std::move(pf.future);
    }

    bool cancelSession(Session& session) noexcept override {
        const auto id = session.id();

        stdx::unique_lock<Latch> lk(_mutex);

        if (_sessions.find(id) == _sessions.end()) {
            return false;
        }

        _safeExecute(std::move(lk), [id, this] { _sessions.erase(id); });

        return true;
    }

    bool cancelTimer(const ReactorTimer& timer) noexcept override {
        const auto id = timer.id();

        stdx::unique_lock<Latch> lk(_mutex);

        if (_timersById.find(id) == _timersById.end()) {
            return false;
        }

        _safeExecute(std::move(lk), [id, this] {
            auto iter = _timersById.find(id);

            if (iter != _timersById.end()) {
                _timers.erase(iter->second);
                _timersById.erase(iter);
            }
        });

        return true;
    }

    void schedule(Task func) noexcept override {
        stdx::lock_guard<Latch> lk(_mutex);

        if (!_opCtx) {
            func(kDetached);

            return;
        }

        _scheduled.push_back(std::move(func));

        if (_inPoll) {
            efd().notify();
        }
    }

    void notify() noexcept override {
        efd().notify();
    }

    /**
     * We synthesize a run_until by creating a synthetic timer which we use to exit run early (we
     * create a regular waitUntil baton event off the timer, with the passed deadline).
     */
    Waitable::TimeoutState run_until(ClockSource* clkSource, Date_t deadline) noexcept override {
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

    void run(ClockSource* clkSource) noexcept override {
        std::vector<Promise<void>> toFulfill;

        // We'll fulfill promises and run jobs on the way out, ensuring we don't hold any locks
        const auto guard = makeGuard([&] {
            for (auto& promise : toFulfill) {
                promise.emplaceValue();
            }

            stdx::unique_lock<Latch> lk(_mutex);
            while (_scheduled.size()) {
                auto toRun = std::exchange(_scheduled, {});

                lk.unlock();
                for (auto& job : toRun) {
                    job(Status::OK());
                }
                lk.lock();
            }
        });

        stdx::unique_lock<Latch> lk(_mutex);

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

        _pollSet.push_back(pollfd{efd().fd, POLLIN, 0});

        for (auto iter = _sessions.begin(); iter != _sessions.end(); ++iter) {
            _pollSet.push_back(pollfd{iter->second.fd, iter->second.type, 0});
            _pollSessions.push_back(iter);
        }

        auto now = clkSource->now();

        int rval = 0;
        // If we don't have a timeout, or we have a timeout that's unexpired, run poll.
        if (!deadline || (*deadline > now)) {
            if (deadline && !clkSource->tracksSystemClock()) {
                invariant(clkSource->setAlarm(*deadline, [this] { notify(); }));

                deadline.reset();
            }

            _inPoll = true;
            lk.unlock();
            rval = ::poll(_pollSet.data(),
                          _pollSet.size(),
                          deadline ? Milliseconds(*deadline - now).count() : -1);

            const auto pollGuard = makeGuard([&] {
                lk.lock();
                _inPoll = false;
            });

            // If poll failed, it better be in EINTR
            if (rval < 0 && errno != EINTR) {
                severe() << "error in poll: " << errnoWithDescription(errno);
                fassertFailed(50834);
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
                efd().wait();

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

private:
    Future<void> addSessionImpl(Session& session, short type) noexcept {
        auto fd = checked_cast<ASIOSession&>(session).getSocket().native_handle();
        auto id = session.id();
        auto pf = makePromiseFuture<void>();

        stdx::unique_lock<Latch> lk(_mutex);

        if (!_opCtx) {
            return kDetached;
        }

        _safeExecute(std::move(lk),
                     [id, fd, type, promise = std::move(pf.promise), this]() mutable {
                         _sessions[id] = TransportSession{fd, type, std::move(promise)};
                     });

        return std::move(pf.future);
    }

    void detachImpl() noexcept override {
        decltype(_sessions) sessions;
        decltype(_scheduled) scheduled;
        decltype(_timers) timers;

        {
            stdx::lock_guard<Latch> lk(_mutex);

            invariant(_opCtx->getBaton().get() == this);
            _opCtx->setBaton(nullptr);

            _opCtx = nullptr;

            using std::swap;
            swap(_sessions, sessions);
            swap(_scheduled, scheduled);
            swap(_timers, timers);
        }

        for (auto& job : scheduled) {
            job(kDetached);
        }

        for (auto& session : sessions) {
            session.second.promise.setError(kDetached);
        }

        for (auto& pair : timers) {
            pair.second.promise.setError(kDetached);
        }
    }

    struct Timer {
        Timer(size_t id, Promise<void> promise) : id(id), promise(std::move(promise)) {}

        size_t id;
        Promise<void> promise;  // Needs to be mutable to move from it while in std::set.
    };

    struct TransportSession {
        int fd;
        short type;
        Promise<void> promise;
    };

    /**
     * Safely executes method on the reactor.  If we're in poll, we schedule a task, then write to
     * the eventfd.  If not, we run inline.
     */
    template <typename Callback>
    void _safeExecute(stdx::unique_lock<Latch> lk, Callback&& cb) {
        if (_inPoll) {
            _scheduled.push_back([cb = std::forward<Callback>(cb), this](Status) mutable {
                stdx::lock_guard<Latch> lk(_mutex);
                cb();
            });

            efd().notify();
        } else {
            cb();
        }
    }

    EventFDHolder& efd() {
        return EventFDHolder::getForClient(_opCtx->getClient());
    }

    Mutex _mutex = MONGO_MAKE_LATCH("BatonASIO::_mutex");

    OperationContext* _opCtx;

    bool _inPoll = false;

    // This map stores the sessions we need to poll on. We unwind it into a pollset for every
    // blocking call to run
    stdx::unordered_map<SessionId, TransportSession> _sessions;

    // The set is used to find the next timer which will fire.  The unordered_map looks up the
    // timers so we can remove them in O(1)
    std::multimap<Date_t, Timer> _timers;
    stdx::unordered_map<size_t, decltype(_timers)::const_iterator> _timersById;

    // For tasks that come in via schedule.  Or that were deferred because we were in poll
    std::vector<Task> _scheduled;

    // We hold the two following values at the object level to save on allocations when a baton is
    // waited on many times over the course of its lifetime.

    // Holds the pollset for ::poll
    std::vector<pollfd> _pollSet;

    // Mirrors the above pollset with mappings back to _sessions
    std::vector<decltype(_sessions)::iterator> _pollSessions;
};

const Client::Decoration<TransportLayerASIO::BatonASIO::EventFDHolder>
    TransportLayerASIO::BatonASIO::EventFDHolder::getForClient =
        Client::declareDecoration<TransportLayerASIO::BatonASIO::EventFDHolder>();

}  // namespace transport
}  // namespace mongo
