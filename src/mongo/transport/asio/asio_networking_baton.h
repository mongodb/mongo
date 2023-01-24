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

#include "mongo/platform/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/transport/asio/asio_session.h"
#include "mongo/transport/baton.h"
#include "mongo/util/concepts.h"
#include "mongo/util/functional.h"
#include "mongo/util/future.h"
#include "mongo/util/hierarchical_acquisition.h"

namespace mongo {
namespace transport {

/**
 * AsioTransportLayer Baton implementation for linux.
 *
 * We implement our networking reactor on top of poll + eventfd for wakeups
 */
class AsioNetworkingBaton : public NetworkingBaton {
public:
    AsioNetworkingBaton(OperationContext* opCtx) : _opCtx(opCtx) {}

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

    Future<void> waitUntil(Date_t expiration, const CancellationToken&) noexcept override;

    bool cancelSession(Session& session) noexcept override;

    bool cancelTimer(const ReactorTimer& timer) noexcept override;

    bool canWait() noexcept override;

private:
    struct Timer {
        size_t id;  // Stores the unique identifier for the timer, provided by `ReactorTimer`.
        Promise<void> promise;
    };

    struct TransportSession {
        int fd;
        short events;  // Events to consider while polling for this session (e.g., `POLLIN`).
        Promise<void> promise;
    };

    bool _cancelTimer(size_t timerId) noexcept;

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
     * Returns the list of promises that must be fulfilled as the result of polling.
     */
    std::list<Promise<void>> _poll(stdx::unique_lock<Mutex>&, ClockSource*);

    Future<void> _addSession(Session& session, short events);

    void detachImpl() noexcept override;

    Mutex _mutex = MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(0), "AsioNetworkingBaton::_mutex");

    OperationContext* _opCtx;

    bool _inPoll = false;

    // Stores the sessions we need to poll on.
    stdx::unordered_map<SessionId, TransportSession> _sessions;

    /**
     * We use two structures to maintain timers:
     * - `_timers` keeps a sorted list of timers according to their expiration date.
     * - `_timersById` allows using the unique timer id to find and cancel a timer in constant time.
     */
    std::multimap<Date_t, Timer> _timers;
    stdx::unordered_map<size_t, std::multimap<Date_t, Timer>::iterator> _timersById;

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
};

}  // namespace transport
}  // namespace mongo
