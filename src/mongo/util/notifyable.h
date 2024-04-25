/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/platform/atomic.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/with_lock.h"

namespace mongo {

/**
 * Notifyable is a slim type meant to allow integration of special kinds of waiters in a
 * NotifyableParkingLot. Specifially, the notify() on this type will be called directly from
 * NotifyableParkingLot::notify(One|Some|All).
 *
 * See Waitable for how we use this in the stdx::condition_variable integration.
 */
class Notifyable {
    friend class NotifyableParkingLot;

public:
    Notifyable() = default;

    // !!! PAY ATTENTION, THERE IS DANGER HERE !!!
    //
    // Implementers of the notifyable api must be level triggered by notify, rather than edge
    // triggered.
    //
    // I.e. a call to notify() must either unblock the notifyable immediately, if it is currently
    // blocked, or unblock it the next time it would wait, if it is not currently blocked.
    //
    // In addition to unblocking, the notifyable should also atomically consume that notification
    // state as a result of waking.  I.e. any number of calls to notify before or during a wait must
    // unblock exactly one wait.
    //
    // Notifyable::notify is not like condition_variable::notify_X()
    virtual void notify() noexcept = 0;

protected:
    ~Notifyable() = default;

private:
    // Notifyable's own a list node which they splice into a NotifyableParkingLot's wait list when
    // waiting on one. It looks like we have a private member list of Notifyable*, but it's written
    // this way because `std::list` does not have a `node_handle` type.  It's important that we
    // pre-allocate this entry on construction.
    //
    // Note that the Notifyable** in this list is only used and meaningful while the Notifyable
    // waits on a Waitable (so the ownership of a self pointer doesn't really have implications for
    // copy/move, as the objects shouldn't be moved/copied while waiting)
    std::list<Notifyable*> _handleContainer{this};
};

/**
 * A type you can use to park Notifyable's in a queue, awaiting level-triggered notification from
 * another thread.
 */
class NotifyableParkingLot {
public:
    /**
     * Notify one queued waiter, if one exists. Returns true if a waiter was notified.
     */
    bool notifyOne() noexcept {
        if (_notifyableCount.load()) {
            stdx::lock_guard lk(_mutex);
            return _notifyNextNotifyable(lk);
        }

        return false;
    }

    /**
     * Notify some queued waiters, if they exist. Returns true if some waiters were notified.
     */
    bool notifySome(uint64_t nToWake) noexcept {
        uint64_t waiterCount = _notifyableCount.load();
        if (waiterCount) {
            int count = std::min(waiterCount, nToWake);
            stdx::lock_guard lk(_mutex);
            for (auto i = 0; i < count; ++i) {
                _notifyNextNotifyable(lk);
            }

            return true;
        }

        return false;
    }

    /**
     * Notify all queued waiters.
     */
    void notifyAll() noexcept {
        if (_notifyableCount.load()) {
            stdx::lock_guard lk(_mutex);
            while (_notifyNextNotifyable(lk)) {
            }
        }
    }

    /**
     * Park the notifyable in the queue of waiters for this parking lot, and run the provided
     * callback. This ensures that for the duration of the callback execution, a call to the
     * notification primitives on this parking lot will trigger a notify() on the Notifyable.
     *
     * The scheme here is that list entries are spliced back to their Notifyable from the
     * notification list when the parking lot is notified (so that they don't consume multiple
     * `notifyOne`'s).  We detect that condition by noting that our list isn't empty (in which case
     * we should avoid a double splice back).
     *
     * NOTE: This method must not be called from multiple threads with the same notifyable.
     */
    template <typename Callback>
    void runWithNotifyable(Notifyable& notifyable, Callback&& cb) noexcept {
        static_assert(noexcept(std::forward<Callback>(cb)()),
                      "Only noexcept functions may be invoked with runWithNotifyable");

        auto iter = [&] {
            stdx::lock_guard localMutex(_mutex);
            _notifyableCount.addAndFetch(1);
            invariant(!notifyable._handleContainer.empty());
            _notifyables.splice(_notifyables.end(),
                                notifyable._handleContainer,
                                notifyable._handleContainer.begin());
            return --_notifyables.end();
        }();

        // The supplied callback must perform a wait which returns when the `Notifyable` is
        // notified, as well as any other work the waiter would like to do while waiting.
        std::forward<Callback>(cb)();

        stdx::lock_guard localMutex(_mutex);

        // If our list isn't empty, we were notified and spliced back in `_notifyNextNotifyable`.
        // If it is empty, we need to stash our wait queue iterator ourselves.
        if (notifyable._handleContainer.empty()) {
            _notifyableCount.subtractAndFetch(1);
            _spliceBack(localMutex, iter);
        }
    }

private:
    /**
     * Notifies the next notifyable.
     *
     * Returns true if there was a notifyable to be notified.
     *
     * Note that as part of notifying, we splice back iterators to `_runWithNotifyable` callers.
     * This is safe because we hold `_mutex` while we do so, and our splicing communicates that
     * those waiters need not clear themselves from the notification list on wakeup.
     */
    bool _notifyNextNotifyable(WithLock wl) noexcept {
        auto iter = _notifyables.begin();
        if (iter == _notifyables.end()) {
            return false;
        }

        _notifyableCount.subtractAndFetch(1);

        (*iter)->notify();

        _spliceBack(wl, iter);

        return true;
    }

    /**
     * Splice the Notifyable iterator back into the Notifyable (out from this parking lot's wait
     * list).
     */
    void _spliceBack(WithLock, std::list<Notifyable*>::iterator iter) {
        auto notifyable = *iter;
        notifyable->_handleContainer.splice(
            notifyable->_handleContainer.begin(), _notifyables, iter);
    }

    stdx::mutex _mutex;  // NOLINT
    std::list<Notifyable*> _notifyables;
    Atomic<uint64_t> _notifyableCount;
};

}  // namespace mongo
