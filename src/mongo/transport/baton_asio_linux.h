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
#include "mongo/util/concepts.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/functional.h"
#include "mongo/util/future.h"
#include "mongo/util/hierarchical_acquisition.h"
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
    static const inline auto kCanceled =
        Status(ErrorCodes::CallbackCanceled, "Baton wait canceled");

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

    Future<void> waitUntil(const ReactorTimer& timer, Date_t expiration) noexcept override try {
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

    bool canWait() noexcept override {
        stdx::lock_guard lk(_mutex);
        return _opCtx;
    }

    bool cancelSession(Session& session) noexcept override {
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

            session.promise.setError(kCanceled);
        });

        return true;
    }

    bool cancelTimer(const ReactorTimer& timer) noexcept override {
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

            timer.promise.setError(kCanceled);
        });

        return true;
    }

    void schedule(Task func) noexcept override {
        stdx::unique_lock lk(_mutex);

        if (!_opCtx) {
            lk.unlock();
            func(kDetached);

            return;
        }

        _scheduled.push_back(
            [ this, func = std::move(func) ](stdx::unique_lock<Mutex> lk) mutable noexcept {
                auto status = Status::OK();
                if (!_opCtx) {
                    status = kDetached;
                }
                lk.unlock();

                func(status);
            });

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
                invariant(clkSource->setAlarm(*deadline,
                                              [this, anchor = shared_from_this()] { notify(); }));

                deadline.reset();
            }

            _inPoll = true;
            lk.unlock();
            rval = ::poll(_pollSet.data(),
                          _pollSet.size(),
                          deadline ? Milliseconds(*deadline - now).count() : -1);
            lk.lock();
            _inPoll = false;

            // If poll failed, it better be in EINTR
            if (rval < 0 && errno != EINTR) {
                LOGV2_FATAL(50834,
                            "error in poll: {errnoWithDescription_errno}",
                            "errnoWithDescription_errno"_attr = errnoWithDescription(errno));
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
    Future<void> addSessionImpl(Session& session, short type) noexcept try {
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

    void detachImpl() noexcept override {
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
            session.second.promise.setError(kDetached);
        }

        for (auto& pair : timers) {
            pair.second.promise.setError(kDetached);
        }
    }

    struct Timer {
        size_t id;
        Promise<void> promise;  // Needs to be mutable to move from it while in std::set.
    };

    struct TransportSession {
        int fd;
        short type;
        Promise<void> promise;
    };

    // Internally, the BatonASIO thinks in terms of synchronized units of work. This is because
    // a Baton effectively represents a green thread with the potential to add or remove work (i.e.
    // Jobs) at any time. Jobs with external notifications (OutOfLineExecutor::Tasks,
    // TransportSession:promise, ReactorTimer::promise) are expected to release their lock before
    // generating those notifications.
    using Job = unique_function<void(stdx::unique_lock<Mutex>)>;

    /**
     * Invoke a job with exclusive access to the Baton internals.
     *
     * If we are currently _inPoll, the polling thread owns the Baton and thus we tell it to wake up
     * and run our job. If we are not _inPoll, take exclusive access and run our job on the local
     * thread. Note that _safeExecute() will throw if the Baton has been detached.
     */
    TEMPLATE(typename Callback)
    REQUIRES(std::is_nothrow_invocable_v<Callback, stdx::unique_lock<Mutex>>)
    void _safeExecute(stdx::unique_lock<Mutex> lk, Callback&& job) {
        if (!_opCtx) {
            // If we're detached, no job can safely execute.
            uassertStatusOK(kDetached);
        }

        if (_inPoll) {
            _scheduled.push_back(std::forward<Callback>(job));

            efd().notify();
        } else {
            job(std::move(lk));
        }
    }

    EventFDHolder& efd() {
        return EventFDHolder::getForClient(_opCtx->getClient());
    }

    Mutex _mutex = MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(0), "BatonASIO::_mutex");

    OperationContext* _opCtx;

    bool _inPoll = false;

    // This map stores the sessions we need to poll on. We unwind it into a pollset for every
    // blocking call to run
    stdx::unordered_map<SessionId, TransportSession> _sessions;

    // The set is used to find the next timer which will fire.  The unordered_map looks up the
    // timers so we can remove them in O(1)
    std::multimap<Date_t, Timer> _timers;
    stdx::unordered_map<size_t, decltype(_timers)::iterator> _timersById;

    // For tasks that come in via schedule.  Or that were deferred because we were in poll
    std::vector<Job> _scheduled;

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
