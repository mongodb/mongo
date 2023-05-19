/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

#include "ticket_pool.h"

// TODO SERVER-72616: Remove futex usage from this class in favour of atomic waits.
#include <linux/futex.h> /* Definition of FUTEX_* constants */
#include <sys/syscall.h> /* Definition of SYS_* constants */
#include <unistd.h>

#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/errno_util.h"

namespace mongo {
namespace {
static int futex(uint32_t* uaddr,
                 int futex_op,
                 uint32_t val,
                 const struct timespec* timeout,
                 uint32_t* uaddr2,
                 uint32_t val3) noexcept {
    return syscall(SYS_futex, uaddr, futex_op, val, timeout, uaddr2, val3);
}

// TODO SERVER-72616: This can go away once we're on C++20 and std::atomic<T>::wait exists
static stdx::cv_status atomic_wait(AtomicWord<uint32_t>& atomic,
                                   uint32_t expectedValue,
                                   Date_t until) noexcept {
    while (atomic.load() == expectedValue) {
        // Prepare the timeout value for the futex call.
        timespec ts;
        auto now = Date_t::now();
        if (now >= until) {
            return stdx::cv_status::timeout;
        }
        auto millis = until - now;
        ts.tv_sec = millis.count() / 1'000;
        ts.tv_nsec = (millis.count() % 1'000) * 1'000'000;

        auto futexResult = futex(reinterpret_cast<uint32_t*>(&atomic),
                                 FUTEX_WAIT_PRIVATE,
                                 expectedValue,
                                 &ts,
                                 nullptr,
                                 0);
        if (futexResult != 0) {
            switch (errno) {
                // The value has changed before we called futex wait, we treat this as a
                // notification and exit.
                case EAGAIN:
                    return stdx::cv_status::no_timeout;
                case ETIMEDOUT:
                    return stdx::cv_status::timeout;
                // We ignore signal interruptions as other signals are handled by either crashing
                // the server or gracefully exiting the server and waiting for operations to finish.
                case EINTR:
                    break;
                // All other errors are unrecoverable, fassert and crash the server.
                default: {
                    LOGV2_FATAL(7206704,
                                "Error in atomic wait for ticket",
                                "error"_attr = errorMessage(posixError(errno)));
                }
            }
        }
    }
    return stdx::cv_status::no_timeout;
}

// TODO SERVER-72616: This can go away once we're on C++20 and std::atomic<T>::notify_one exists.
static void atomic_notify_one(AtomicWord<uint32_t>& atomic) noexcept {
    auto result =
        futex(reinterpret_cast<uint32_t*>(&atomic), FUTEX_WAKE_PRIVATE, 1, nullptr, nullptr, 0);
    if (result < 0) {
        // We treat possible errors here as a server crash since we cannot recover from them.
        LOGV2_FATAL(7206703,
                    "Error in atomic notify for ticket",
                    "error"_attr = errorMessage(posixError(errno)));
    }
}
}  // namespace

template <class Queue>
bool TicketPool<Queue>::tryAcquire() {
    auto available = _available.load();
    bool gotTicket = false;
    while (available > 0 && !gotTicket) {
        gotTicket = _available.compareAndSwap(&available, available - 1);
    }
    return gotTicket;
}

template <class Queue>
bool TicketPool<Queue>::acquire(AdmissionContext* admCtx, Date_t deadline) {
    auto waiter = std::make_shared<TicketWaiter>();
    waiter->context = admCtx;

    {
        stdx::unique_lock<Mutex> lk(_mutex);
        // It is important to check for a ticket one more time before queueing, as a ticket may have
        // just become available.
        if (tryAcquire()) {
            return true;
        }
        _waiters.push(waiter);
    }
    _queued.addAndFetch(1);

    auto res = atomic_wait(waiter->futexWord, TicketWaiter::State::Waiting, deadline);
    if (res == stdx::cv_status::timeout) {
        // If we timed out, we need to invalidate ourselves, but ensure that we take a ticket if
        // it was given.
        auto state = static_cast<uint32_t>(TicketWaiter::State::Waiting);
        if (waiter->futexWord.compareAndSwap(&state, TicketWaiter::State::TimedOut)) {
            // Successfully set outselves to timed out so nobody tries to give us a ticket.
            return false;
        } else {
            // We were given a ticket anyways. We must take it.
            invariant(state == TicketWaiter::State::Acquired);
            return true;
        }
    }
    invariant(waiter->futexWord.load() == TicketWaiter::State::Acquired);
    return true;
}

template <class Queue>
std::shared_ptr<TicketWaiter> TicketPool<Queue>::_popWaiterOrAddTicketToPool() {
    stdx::unique_lock<Mutex> lock(_mutex);
    auto waiter = _waiters.pop();
    if (!waiter) {
        // We need to ensure we add the ticket back to the pool while holding the mutex. This
        // prevents a soon-to-be waiter from missing an available ticket. Otherwise, we could
        // leave a waiter in the queue without ever waking it.
        _available.addAndFetch(1);
    }
    return waiter;
}

template <class Queue>
void TicketPool<Queue>::_release() {
    while (auto waiter = _popWaiterOrAddTicketToPool()) {
        _queued.subtractAndFetch(1);
        auto state = static_cast<uint32_t>(TicketWaiter::State::Waiting);
        if (waiter->futexWord.compareAndSwap(&state, TicketWaiter::State::Acquired)) {
            atomic_notify_one(waiter->futexWord);
            return;
        } else {
            // We raced with the waiter timing out, so we didn't transfer the ticket. Try again.
            invariant(state == TicketWaiter::State::TimedOut);
        }
    }
}

template <class Queue>
void TicketPool<Queue>::release() {
    _release();
}

template class TicketPool<FifoTicketQueue>;
template class TicketPool<SimplePriorityTicketQueue>;

}  // namespace mongo
