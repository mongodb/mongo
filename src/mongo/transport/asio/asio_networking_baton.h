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

#include <list>
#include <map>
#include <memory>
#include <vector>

#include <poll.h>

#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/platform/waitable_atomic.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/transport/asio/asio_session.h"
#include "mongo/transport/baton.h"
#include "mongo/util/functional.h"
#include "mongo/util/future.h"
#include "mongo/util/hierarchical_acquisition.h"

namespace mongo {
namespace transport {

class TransportLayer;

/**
 * AsioTransportLayer Baton implementation for linux.
 *
 * We implement our networking reactor on top of poll + eventfd for wakeups
 */
class AsioNetworkingBaton : public NetworkingBaton {
public:
    AsioNetworkingBaton(const TransportLayer* tl, OperationContext* opCtx)
        : _opCtx(opCtx), _tl(tl) {}

    ~AsioNetworkingBaton() {
        invariant(!_opCtx);
        invariant(_sessions.empty());
        invariant(_scheduled.empty());
        invariant(_timers.empty());
    }

    // Overrides for `OutOfLineExecutor`
    void schedule(Task func) noexcept override;

    // Overrides for `Waitable` and `Notifyable`
    void notify() noexcept override;

    Waitable::TimeoutState run_until(ClockSource* clkSource, Date_t deadline) noexcept override;

    void run(ClockSource* clkSource) noexcept override;

    // Overrides for `Baton`
    void markKillOnClientDisconnect() noexcept override;

    // Overrides for `NetworkingBaton`
    Future<void> addSession(Session& session, Type type) noexcept override;

    Future<void> waitUntil(const ReactorTimer& timer, Date_t expiration) noexcept override;

    Future<void> waitUntil(Date_t expiration, const CancellationToken&) override;

    /**
     * Cancellations are not necessarily processed in order. For example, consider:
     * Baton someBaton;
     * someBaton.addSession(S1); someBaton.addSession(S2);
     * someBaton.cancelSession(S1); someBaton.cancelSession(S2);
     * The continuation for `S1` may run before or after that of `S2`. Continuations for
     * timers behave similarly with respect to cancellation.
     */
    bool cancelSession(Session& session) noexcept override;

    bool cancelTimer(const ReactorTimer& timer) noexcept override;

    bool canWait() noexcept override;

    const TransportLayer* getTransportLayer() const override {
        return _tl;
    }

private:
    struct Timer {
        size_t id;  // Stores the unique identifier for the timer, provided by `ReactorTimer`.
        Promise<void> promise;
        bool canceled = false;
    };

    struct TransportSession {
        int fd;
        short events;  // Events to consider while polling for this session (e.g., `POLLIN`).
        Promise<void> promise;
        bool canceled = false;
    };

    bool _cancelTimer(size_t timerId) noexcept;
    void _addTimer(Date_t expiration, Timer timer);

    /*
     * Internally, `AsioNetworkingBaton` thinks in terms of synchronized units of work. This is
     * because a baton effectively represents a green thread with the potential to add or remove
     * work (i.e., jobs) at any time. Thus, scheduled jobs must release their lock before executing
     * any task external to the baton (e.g., `OutOfLineExecutor::Task`, `TransportSession:promise`,
     * and `ReactorTimer::promise`).
     */
    using Job = unique_function<void(stdx::unique_lock<Mutex>)>;

    /**
     * Invokes a job with exclusive access to the baton's internals.
     *
     * If the baton is currently polling (i.e., `_inPoll` is `true`), the polling thread owns the
     * baton, so we schedule the job and notify the polling thread to wake up and run the job.
     *
     * Otherwise, take exclusive access and run the job on the current thread.
     *
     * Note that `_safeExecute()` will throw if the baton has been detached.
     *
     * Also note that the job may not run inline, and may get scheduled to run by the baton, so it
     * should never throw.
     */
    void _safeExecute(stdx::unique_lock<Mutex> lk, Job job);

    /**
     * Blocks polling on the registered sessions until one of the following happens:
     * - `notify()` is called, either directly or through other methods (e.g., `schedule()`).
     * - One of the timers scheduled on this baton times out.
     * - There is an event for at least one of the registered sessions (e.g., data is available).
     * Returns two lists of promises that must be fulfilled as the result of polling - the first
     * must be fulfilled successfully and the second must be fulfilled with a cancellation error.
     */
    std::pair<std::list<Promise<void>>, std::list<Promise<void>>> _poll(stdx::unique_lock<Mutex>&,
                                                                        ClockSource*);

    Future<void> _addSession(Session& session, short events);

    void detachImpl() noexcept override;

    Mutex _mutex = MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(0), "AsioNetworkingBaton::_mutex");

    OperationContext* _opCtx;

    bool _inPoll = false;

    /**
     * We use `_notificationState` to allow the baton to use different polling/waiting mechanisms
     * (for efficiency) depending on what circumstances allow. Calls to `notify()` take the current
     * state into consideration when deciding which mechanism to use to wake up the polling
     * thread. In all states, notifiers must set the notification state to `kNotificationPending`.
     * If we're in the middle of a call to `::poll` (state `kInPoll`), notifiers must then use
     * the eventfd to to wake the polling thread. If we're in the middle of waiting on
     * `_notificationState` itself (state `kInAtomicWait`), notifiers must notify on
     * `_notificationState`. In state `kNone`, no thread is blocked polling or waiting, so the
     * notifier only needs to set the state to `kNotificationPending`. Finally, in state
     * `kNotificationPending`, `notify()` is a no-op.
     *
     * In `notify()`, a thread sending a notification can cause a transition from any state to
     * `kNotificationPending`. Graphically:
     *
     * kNone ----1----> kNotificationPending <----2---- kInPoll          kInAtomicWait
     *                                   ^                                  |
     *                                   +-------------------------3--------+
     * (1) The polling thread is not blocked - just transition to `kNotificationPending`.
     * (2) The polling thread may be blocked in `::poll()` - notify the eventfd.
     * (3) The polling thread may be blocked on `_notificationState` - notify it.
     *
     * In `_poll()`, if there are active sessions, the polling thread transitions to `kInPoll`
     * (from `kNone` or `kNotificationPending`) before calling `::poll()`, and transitions to
     * `kNone` after `::poll()` returns. Alternatively, when there are no active sessions and no
     * pending notifications, the polling thread instead transitions from `kNone` to
     * `kInAtomicWait` and waits on `_notificationState`. There is also a fast path from
     * `kNotificationPending` to `kNone` when a notification is pending and there are no sessions
     * to poll on.
     *
     * +-------+ -----2-----> +---------+            +----------------------+
     * | kNone |              | kInPoll | <----3---- | kNotificationPending |
     * +-------+ <----1------ +---------+            +----------------------+
     *  |     ^                                           |
     *  5     +-----------------4-------------------------+
     *  V
     * +---------------+
     * | kInAtomicWait |
     * +---------------+
     *
     * (1) Return from `::poll()`
     * (2) Start polling with no pending notification and at least one active session
     *      - use a blocking call to `::poll()`.
     * (3) Start polling with a pending notification and at least one active session
     *      - use a non-blocking call to `::poll()`.
     * (4) Start polling with a pending notification and no active sessions
     *      - go straight to `kNone` without blocking.
     * (5) Start polling with no pending notification and no active sessions
     *      - wait on `_notificationState`.
     *
     * Notice that only notifying threads transition to `kNotificationPending` and only the polling
     * thread transitions out of `kNotificationPending`. This gives the polling thread exclusive
     * ownership over `_notificationState` in that state (i.e., no one else will write to it).
     *
     * `_notificationState` is a BasicWaitableAtomic because the state tells us if anyone is
     * waiting. The only time there might be a waiter is in `kInAtomicWait`, and there is only
     * ever one waiter.
     */
    enum NotificationState : uint32_t { kNone, kNotificationPending, kInPoll, kInAtomicWait };
    BasicWaitableAtomic<NotificationState> _notificationState;

    /**
     * Stores the sessions we need to poll on.
     * `_pendingSessions` stores sessions that have been added, but due to an ongoing poll, haven't
     * been added to `_sessions` yet. The baton only starts polling on a session once it gets
     * added to `_sessions`.
     */
    stdx::unordered_map<SessionId, TransportSession> _sessions;
    stdx::unordered_map<SessionId, TransportSession> _pendingSessions;

    /**
     * We use three structures to maintain timers:
     * - `_timers` keeps a sorted list of timers according to their expiration date.
     * - `_timersById` allows using the unique timer id to find and cancel a timer in constant time.
     * - `_pendingTimers` keeps a map from timer id to (timer, expiration) pairs that haven't
     *   been added to the other two members yet due to an ongoing `_poll`.
     * Timers that are in `_pendingTimers` won't fire upon expiration until they are added to
     * `_timers` and `_timersById`.
     */
    std::multimap<Date_t, Timer> _timers;
    stdx::unordered_map<size_t, std::multimap<Date_t, Timer>::iterator> _timersById;
    stdx::unordered_map<size_t, std::pair<Date_t, Timer>> _pendingTimers;

    // Tasks scheduled for deferred execution.
    std::vector<Job> _scheduled;

    /*
     * We hold the following values at the object level to save on allocations when a baton is
     * waited on many times over the course of its lifetime:
     * `_pollSet`: the poll set for `::poll`.
     * `_pollSessions`: maps members of `_pollSet` to their corresponding session in `_sessions`.
     */
    std::vector<::pollfd> _pollSet;
    std::vector<decltype(_sessions)::iterator> _pollSessions;

    const TransportLayer* _tl;
};

}  // namespace transport
}  // namespace mongo
