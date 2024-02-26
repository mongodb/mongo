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


#include "mongo/transport/asio/asio_networking_baton.h"

#include <sys/eventfd.h>

#include "mongo/base/checked_cast.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/operation_context.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler_gcc.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo {
namespace transport {
namespace {

MONGO_FAIL_POINT_DEFINE(blockAsioNetworkingBatonBeforePoll);

Status getDetachedError() {
    return {ErrorCodes::ShutdownInProgress, "Baton detached"};
}

Status getCanceledError() {
    return {ErrorCodes::CallbackCanceled, "Baton wait canceled"};
}

/**
 * RAII type that wraps up an `eventfd` and reading/writing to it.
 * We don't use the counter portion and only use the file descriptor (i.e., `fd`) to notify and
 * interrupt the client thread blocked polling (see `AsioNetworkingBaton::run`).
 */
struct EventFDHolder {
    EventFDHolder(const EventFDHolder&) = delete;
    EventFDHolder& operator=(const EventFDHolder&) = delete;

    EventFDHolder() = default;

    ~EventFDHolder() {
        ::close(fd);
    }

    void notify() {
        while (::eventfd_write(fd, 1) != 0) {
            const auto savedErrno = errno;
            if (savedErrno == EINTR)
                continue;
            LOGV2_FATAL(6328202, "eventfd write failed", "fd"_attr = fd, "errno"_attr = savedErrno);
        }
    }

    void wait() {
        // If we have activity on the `eventfd`, pull the count out.
        ::eventfd_t u;
        while (::eventfd_read(fd, &u) != 0) {
            const auto savedErrno = errno;
            if (savedErrno == EINTR)
                continue;
            LOGV2_FATAL(6328203, "eventfd read failed", "fd"_attr = fd, "errno"_attr = savedErrno);
        }
    }

private:
    static int _initFd() {
        int fd = ::eventfd(0, EFD_CLOEXEC);
        // On error, -1 is returned and `errno` is set
        if (fd < 0) {
            auto ec = lastPosixError();
            const auto errorCode = ec == posixError(EMFILE) || ec == posixError(ENFILE)
                ? ErrorCodes::TooManyFilesOpen
                : ErrorCodes::UnknownError;
            Status status(errorCode,
                          fmt::format("error in creating eventfd: {}, errno: {}",
                                      errorMessage(ec),
                                      ec.value()));
            LOGV2_ERROR(6328201, "Unable to create eventfd object", "error"_attr = status);
            iasserted(status);
        }
        return fd;
    }

public:
    const int fd = _initFd();
};

const auto getEventFDForClient = Client::declareDecoration<EventFDHolder>();

EventFDHolder& efd(OperationContext* opCtx) {
    return getEventFDForClient(opCtx->getClient());
}

/**
 * This is only used by `run_until()` and `waitUntil()`, and provides a unique timer id. This unique
 * id is supplied by `ReactorTimer`, and used by the baton for internal bookkeeping.
 */
class DummyTimer final : public ReactorTimer {
public:
    void cancel(const BatonHandle& baton = nullptr) override {
        MONGO_UNREACHABLE;
    }

    Future<void> waitUntil(Date_t timeout, const BatonHandle& baton = nullptr) override {
        MONGO_UNREACHABLE;
    }
};

}  // namespace

void AsioNetworkingBaton::schedule(Task func) noexcept {
    auto task = [this, func = std::move(func)](stdx::unique_lock<Mutex> lk) mutable {
        auto status = _opCtx ? Status::OK() : getDetachedError();
        lk.unlock();
        func(std::move(status));
    };

    stdx::unique_lock lk(_mutex);
    if (!_opCtx) {
        // Run the task inline if the baton is detached.
        task(std::move(lk));
        return;
    }

    _scheduled.push_back(std::move(task));
    if (_inPoll)
        notify();
}

void AsioNetworkingBaton::notify() noexcept {
    NotificationState old = _notificationState.swap(kNotificationPending);
    if (old == kInAtomicWait)
        _notificationState.notifyAll();
    else if (old == kInPoll)
        efd(_opCtx).notify();
}

Waitable::TimeoutState AsioNetworkingBaton::run_until(ClockSource* clkSource,
                                                      Date_t deadline) noexcept {
    // Set up a timer on the baton with the specified deadline. This synthetic timer is used by
    // `_poll()`, which is called through `run()`, to enforce a deadline for the blocking `::poll`.
    DummyTimer timer;
    auto future = waitUntil(timer, deadline);

    run(clkSource);

    // If the future is ready, our timer interrupted `run()`, in which case we timed out.
    if (future.isReady()) {
        future.get();
        return Waitable::TimeoutState::Timeout;
    } else {
        cancelTimer(timer);
        return Waitable::TimeoutState::NoTimeout;
    }
}

void AsioNetworkingBaton::run(ClockSource* clkSource) noexcept {
    // On the way out, fulfill promises and run scheduled jobs without holding the lock.
    std::list<Promise<void>> toFulfill, toCancel;

    const ScopeGuard guard([&] {
        for (auto& promise : toFulfill) {
            promise.emplaceValue();
        }

        for (auto& promise : toCancel) {
            promise.setError(getCanceledError());
        }

        auto lk = stdx::unique_lock(_mutex);
        while (!_scheduled.empty()) {
            auto scheduled = std::exchange(_scheduled, {});
            for (auto& job : scheduled) {
                job(std::move(lk));
                job = nullptr;
                lk = stdx::unique_lock(_mutex);
            }
        }
    });

    stdx::unique_lock lk(_mutex);

    // If anything was scheduled, run it now and skip polling and processing timers.
    if (!_scheduled.empty())
        return;

    std::tie(toFulfill, toCancel) = _poll(lk, clkSource);

    // Fire expired timers
    const auto now = clkSource->now();
    for (auto it = _timers.begin(); it != _timers.end() && it->first <= now;
         it = _timers.erase(it)) {
        Timer& timer = it->second;
        if (timer.canceled) {
            toCancel.push_back(std::move(timer.promise));
        } else {
            toFulfill.push_back(std::move(timer.promise));
        }
        _timersById.erase(it->second.id);
    }
}

void AsioNetworkingBaton::markKillOnClientDisconnect() noexcept {
    auto client = _opCtx->getClient();
    invariant(client);
    if (auto session = client->session()) {
        auto code = client->getDisconnectErrorCode();
        _addSession(*session, POLLRDHUP).getAsync([this, code](Status status) {
            if (status.isOK())
                _opCtx->markKilled(code);
        });
    }
}

Future<void> AsioNetworkingBaton::addSession(Session& session, Type type) noexcept {
    return _addSession(session, type == Type::In ? POLLIN : POLLOUT);
}

Future<void> AsioNetworkingBaton::waitUntil(const ReactorTimer& reactorTimer,
                                            Date_t expiration) noexcept try {
    auto pf = makePromiseFuture<void>();
    _addTimer(expiration, Timer{reactorTimer.id(), std::move(pf.promise)});

    return std::move(pf.future);
} catch (const DBException& ex) {
    return ex.toStatus();
}

Future<void> AsioNetworkingBaton::waitUntil(Date_t expiration, const CancellationToken& token) try {
    auto pf = makePromiseFuture<void>();
    DummyTimer dummy;
    const size_t timerId = dummy.id();

    _addTimer(expiration, Timer{timerId, std::move(pf.promise)});

    token.onCancel().thenRunOn(shared_from_this()).getAsync([this, timerId](Status s) {
        if (s.isOK()) {
            _cancelTimer(timerId);
        }
    });

    return std::move(pf.future);
} catch (const DBException& ex) {
    return ex.toStatus();
}

void AsioNetworkingBaton::_addTimer(Date_t expiration, Timer timer) {
    const size_t timerId = timer.id;

    stdx::unique_lock lk(_mutex);

    // The timer could exist already, and we need to assert that it's canceled if so.
    auto it = _timersById.find(timerId);
    invariant(it == _timersById.end() || it->second->second.canceled);

    // The timer must not already be in _pendingTimers - cancellations of pending timers
    // are processed immediately, inline, so if the timer is there, this is a duplicate.
    invariant(_pendingTimers.try_emplace(timerId, expiration, std::move(timer)).second,
              "Tried to add an already-existing timer to the baton");

    // _safeExecute moving the timer from _pendingTimers to _timers.
    _safeExecute(std::move(lk), [this, id = timerId](stdx::unique_lock<Mutex> lk) {
        auto pendingIt = _pendingTimers.find(id);
        // The timer may have been canceled out of _pendingTimers
        if (pendingIt == _pendingTimers.end()) {
            return;
        }

        // The timer may exist in _timers in the canceled state. We need to process its
        // cancellation right now so we can put the new copy in its place.
        boost::optional<Promise<void>> promise;
        if (auto it = _timersById.find(id); it != _timersById.end()) {
            Timer timer = std::exchange(it->second->second, {});
            invariant(timer.canceled, "Tried to overwrite an existing and active timer");
            promise = std::move(timer.promise);
            _timers.erase(it->second);
            _timersById.erase(it);
        }

        // Expiration, Timer pair
        auto pair = std::move(pendingIt->second);
        _pendingTimers.erase(pendingIt);

        // Make the timer active by putting it into _timers and _timersById
        auto it = _timers.emplace(std::move(pair));
        _timersById[it->second.id] = it;

        lk.unlock();
        if (promise)
            promise->setError(getCanceledError());
    });
}

bool AsioNetworkingBaton::cancelSession(Session& session) noexcept {
    const auto id = session.id();

    stdx::unique_lock lk(_mutex);

    // If the session is still pending, cancel it immediately, inline.
    if (auto it = _pendingSessions.find(id); it != _pendingSessions.end()) {
        TransportSession ts = std::exchange(it->second, {});
        invariant(!ts.canceled, "Canceling session in baton failed");
        _pendingSessions.erase(it);
        lk.unlock();
        ts.promise.setError(getCanceledError());
        return true;
    }

    auto it = _sessions.find(id);

    // If the session isn't in _pendingSessions or _sessions, it's unknown to the baton.
    if (it == _sessions.end())
        return false;

    // If the session is in _sessions and has already had its cancellation requested, return
    // false and take no further action. The cancellation will be processed by a scheduled job.
    if (it->second.canceled)
        return false;

    // Mark the session canceled so we handle subsequent requests to cancel this session correctly.
    it->second.canceled = true;

    // The session is active. Remove it and fulfill the promise out-of-line.
    _safeExecute(std::move(lk), [this, id](stdx::unique_lock<Mutex> lk) {
        auto iter = _sessions.find(id);
        // The session may have been removed already elsewhere, and it may have even been added
        // back to the baton. So we may find it's absent or no longer canceled.
        if (iter == _sessions.end() || !iter->second.canceled) {
            return;
        }

        TransportSession ts = std::exchange(iter->second, {});
        _sessions.erase(iter);
        lk.unlock();

        ts.promise.setError(getCanceledError());
    });

    return true;
}

bool AsioNetworkingBaton::cancelTimer(const ReactorTimer& timer) noexcept {
    const auto id = timer.id();
    return _cancelTimer(id);
}

bool AsioNetworkingBaton::_cancelTimer(size_t id) noexcept {
    stdx::unique_lock lk(_mutex);

    // If the timer is pending, cancel it immediately, inline.
    if (auto it = _pendingTimers.find(id); it != _pendingTimers.end()) {
        Timer timer = std::exchange(it->second.second, {});
        _pendingTimers.erase(it);
        lk.unlock();
        timer.promise.setError(getCanceledError());
        return true;
    }

    auto it = _timersById.find(id);
    if (it == _timersById.end())
        return false;

    // If a cancellation was already requested, rely on the existing scheduled job to remove it
    // rather than scheduling a new one.
    Timer& timer = it->second->second;
    if (timer.canceled)
        return false;

    timer.canceled = true;

    // The timer is active. Remove it and fulfill the promise out-of-line.
    _safeExecute(std::move(lk), [this, id](stdx::unique_lock<Mutex> lk) {
        auto iter = _timersById.find(id);
        // The timer may have already been canceled and removed elsewhere.
        if (iter == _timersById.end())
            return;

        Timer batonTimer = std::exchange(iter->second->second, {});
        _timers.erase(iter->second);
        _timersById.erase(iter);

        lk.unlock();
        batonTimer.promise.setError(getCanceledError());
    });

    return true;
}

bool AsioNetworkingBaton::canWait() noexcept {
    stdx::lock_guard lk(_mutex);
    return _opCtx;
}

void AsioNetworkingBaton::_safeExecute(stdx::unique_lock<Mutex> lk, AsioNetworkingBaton::Job job) {
    if (!_opCtx) {
        // If we're detached, no job can safely execute.
        iasserted(getDetachedError());
    }

    if (_inPoll) {
        _scheduled.push_back(std::move(job));
        notify();
    } else {
        job(std::move(lk));
    }
}

std::pair<std::list<Promise<void>>, std::list<Promise<void>>> AsioNetworkingBaton::_poll(
    stdx::unique_lock<Mutex>& lk, ClockSource* clkSource) {
    const auto now = clkSource->now();

    // If we have a timer, then use it to enforce a timeout for polling.
    boost::optional<Date_t> deadline;
    if (!_timers.empty()) {
        deadline = _timers.begin()->first;

        // Don't poll if we have already passed the deadline.
        if (*deadline <= now)
            return {};
    }

    if (deadline && !clkSource->tracksSystemClock()) {
        // The clock source and `::poll` may track time differently, so use the clock source to
        // enforce the timeout.
        clkSource->setAlarm(*deadline, [self = shared_from_this()] { self->notify(); });
        deadline.reset();
    }

    if (_sessions.empty()) {
        // There's a pending notification and there are no sessions. We should just return
        // rather than calling poll().
        if (_notificationState.load() == kNotificationPending) {
            // Stores of kNone can use relaxed memory order because the notifying thread only needs
            // to see the write of kNone; unrelated writes can safely be reordered around it.
            _notificationState.storeRelaxed(kNone);
            return {};
        }
    } else {
        _pollSet.clear();
        _pollSet.reserve(_sessions.size() + 1);
        _pollSet.push_back({efd(_opCtx).fd, POLLIN, 0});

        _pollSessions.clear();
        _pollSessions.reserve(_sessions.size());

        for (auto iter = _sessions.begin(); iter != _sessions.end(); ++iter) {
            _pollSet.push_back({iter->second.fd, iter->second.events, 0});
            _pollSessions.push_back(iter);
        }
    }

    int events = [&] {
        _inPoll = true;
        lk.unlock();

        // Because _inPoll is true, we have ownership over the baton's state and can safely
        // check if _sessions is empty without holding the lock.
        const NotificationState newState = _sessions.empty() ? kInAtomicWait : kInPoll;

        // If the state was previously kNone, then we're going to wait (either in ::poll or
        // using the WaitableAtomic). We need to set the state accordingly.
        auto oldState = kNone;
        // If the old state was kNone, then there was no notification pending.
        bool wasNotificationPending = !_notificationState.compareAndSwap(&oldState, newState);
        invariant(oldState != kInPoll);
        invariant(oldState != kInAtomicWait);

        const ScopeGuard guard([&] {
            // Both consumes a notification (if-any) and mark us as no-longer in poll.
            _notificationState.storeRelaxed(kNone);

            lk.lock();
            _inPoll = false;
        });

        blockAsioNetworkingBatonBeforePoll.pauseWhileSet();

        if (newState == kInPoll) {
            int timeout = wasNotificationPending
                ? 0  // Don't wait if there is a notification pending.
                : deadline ? Milliseconds(*deadline - now).count()
                           : -1;

            int events = ::poll(_pollSet.data(), _pollSet.size(), timeout);
            if (events < 0) {
                auto ec = lastSystemError();
                if (ec != systemError(EINTR))
                    LOGV2_FATAL(50834, "error in poll", "error"_attr = errorMessage(ec));
            }
            return events;
        } else {
            invariant(newState == kInAtomicWait);
            if (wasNotificationPending)
                return 0;

            // Passing Date_t::max() to waitUntil will cause a duration overflow.
            if (deadline && deadline != Date_t::max())
                _notificationState.waitUntil(kInAtomicWait, *deadline);
            else
                _notificationState.wait(kInAtomicWait);
            return 0;
        }
    }();

    if (events <= 0)
        return {};  // Polling was timed out or interrupted.

    auto psit = _pollSet.begin();
    // Consume the notification on the `eventfd` object if there is any.
    if (psit->revents) {
        efd(_opCtx).wait();
        --events;
    }
    ++psit;

    std::list<Promise<void>> toFulfill, toCancel;
    for (auto sit = _pollSessions.begin(); events && sit != _pollSessions.end(); ++sit, ++psit) {
        if (psit->revents) {
            TransportSession& ts = (*sit)->second;
            if (ts.canceled) {
                toCancel.push_back(std::move(ts.promise));
            } else {
                toFulfill.push_back(std::move(ts.promise));
            }
            _sessions.erase(*sit);
            --events;
        }
    }

    invariant(events == 0, "Failed to process all events after going through registered sessions");
    return std::make_pair(std::move(toFulfill), std::move(toCancel));
}

Future<void> AsioNetworkingBaton::_addSession(Session& session, short events) try {
    auto pf = makePromiseFuture<void>();
    TransportSession ts{checked_cast<AsioSession&>(session).getSocket().native_handle(),
                        events,
                        std::move(pf.promise)};
    SessionId id = session.id();

    stdx::unique_lock lk(_mutex);

    // The session could exist in _sessions, and we need to assert that it's canceled if so.
    auto it = _sessions.find(id);
    invariant(it == _sessions.end() || it->second.canceled,
              "Attempted to add duplicate session to baton");

    // The session must not already be in _pendingSessions - cancellations of pending sessions
    // are processed immediately, inline, so if the session is there, this is a duplicate.
    invariant(_pendingSessions.emplace(id, std::move(ts)).second,
              "Tried to add an already existing session");

    // _safeExecute moving the session from _pendingSessions to _sessions.
    _safeExecute(std::move(lk), [this, id](stdx::unique_lock<Mutex> lk) {
        auto it = _pendingSessions.find(id);
        if (it == _pendingSessions.end()) {
            // The session may have been canceled out of _pendingSessions
            return;
        }

        // The session may exist in _sessions in the canceled state. We need to process its
        // cancellation right now so we can put the new copy in its place.
        boost::optional<Promise<void>> promise;
        auto activeIt = _sessions.find(id);
        if (activeIt != _sessions.end()) {
            invariant(activeIt->second.canceled, "Adding session to baton failed");
            promise = std::move(activeIt->second.promise);
            _sessions.erase(activeIt);
        }

        // Pending sessions cannot remain in the baton in the canceled state
        auto nh = _pendingSessions.extract(it);
        invariant(!nh.mapped().canceled, "Pending session in baton found in the canceled state");
        invariant(_sessions.insert(std::move(nh)).inserted, "Adding session to baton failed");

        lk.unlock();
        if (promise)
            promise->setError(getCanceledError());
    });

    return std::move(pf.future);
} catch (const DBException& ex) {
    return ex.toStatus();
}

void AsioNetworkingBaton::detachImpl() noexcept {

    stdx::unique_lock lk(_mutex);

    invariant(_opCtx->getBaton().get() == this);
    _opCtx->setBaton(nullptr);

    _opCtx = nullptr;

    if (MONGO_likely(_scheduled.empty() && _sessions.empty() && _pendingSessions.empty() &&
                     _timers.empty() && _pendingTimers.empty()))
        return;

    using std::swap;
    decltype(_scheduled) scheduled;
    swap(_scheduled, scheduled);
    decltype(_sessions) sessions;
    swap(_sessions, sessions);
    decltype(_pendingSessions) pendingSessions;
    swap(_pendingSessions, pendingSessions);
    decltype(_timers) timers;
    swap(_timers, timers);
    decltype(_pendingTimers) pendingTimers;
    swap(_pendingTimers, pendingTimers);

    lk.unlock();

    for (auto& job : scheduled) {
        job(stdx::unique_lock(_mutex));
        job = nullptr;
    }

    for (auto& session : sessions) {
        session.second.promise.setError(getDetachedError());
    }

    for (auto& pair : pendingSessions) {
        pair.second.promise.setError(getDetachedError());
    }

    for (auto& pair : timers) {
        pair.second.promise.setError(getDetachedError());
    }

    for (auto& pair : pendingTimers) {
        Timer& timer = pair.second.second;
        timer.promise.setError(getDetachedError());
    }
}

}  // namespace transport
}  // namespace mongo
