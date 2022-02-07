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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/transport/baton.h"
#include "mongo/transport/session_asio.h"
#include "mongo/util/concepts.h"
#include "mongo/util/functional.h"
#include "mongo/util/future.h"
#include "mongo/util/hierarchical_acquisition.h"

namespace mongo {
namespace transport {

/**
 * TransportLayerASIO Baton implementation for linux.
 *
 * We implement our networking reactor on top of poll + eventfd for wakeups
 */
class TransportLayerASIO::BatonASIO : public NetworkingBaton {
    /**
     * RAII type that wraps up an eventfd and reading/writing to it.  We don't actually need the
     * counter portion, just the notify/wakeup
     */
    struct EventFDHolder {
        EventFDHolder() : fd(::eventfd(0, EFD_CLOEXEC)) {
            if (fd < 0) {
                auto savedErrno = errno;
                std::string reason = str::stream()
                    << "error in creating eventfd: " << errnoWithDescription(savedErrno);

                auto code = (savedErrno == EMFILE || savedErrno == ENFILE)
                    ? ErrorCodes::TooManyFilesOpen
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

    bool cancelSession(Session& session) noexcept override;

    bool cancelTimer(const ReactorTimer& timer) noexcept override;

    bool canWait() noexcept override;

private:
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
            uassertStatusOK({ErrorCodes::ShutdownInProgress, "Baton detached"});
        }

        if (_inPoll) {
            _scheduled.push_back(std::forward<Callback>(job));

            _efd().notify();
        } else {
            job(std::move(lk));
        }
    }

    EventFDHolder& _efd();

    Future<void> _addSession(Session& session, short type) noexcept;

    void detachImpl() noexcept override;

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

}  // namespace transport
}  // namespace mongo
