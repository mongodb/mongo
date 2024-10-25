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

#include <atomic>
#include <list>

#include "mongo/platform/atomic.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/with_lock.h"

namespace mongo {

/**
 * Notifiable is a slim type meant to allow integration of special kinds of waiters for
 * ParkingLot.  Specifially, the notify() on this type will be called directly from
 * ParkingLot::unpark(One|All).
 */
class Notifiable {
public:
    /**
     * A call to notify() must either unblock the notifiable immediately, if it is currently
     * blocked, or unblock it the next time it would wait, if it is not currently blocked.
     *
     * In addition to unblocking, the notifiable should also atomically consume that notification
     * state as a result of waking.  I.e. any number of calls to notify before or during a wait must
     * unblock exactly one wait.
     *
     * Notifiable::notify is not like condition_variable::notify_X()
     */
    virtual void notify() noexcept = 0;

    /**
     * Returns a mutable reference to the _handleContainer list that is used to store this
     * Notifiable when it is not waiting. Callers such as ParkingLot can use this function
     * to splice this Notifiable in and out of their own list-of-Notifiables to track which
     * Notifiables are waiting.
     */
    std::list<Notifiable*>& getHandleContainer() {
        return _handleContainer;
    }

protected:
    ~Notifiable() = default;

private:
    /**
     * Notifiable's own a list node which they splice into a ParkingLot's wait list
     * when waiting on one.  It's important that we pre-allocate this entry on construction.

     * Note that the notifiable in this list is only used and meaningful while the notifiable
     * is parked in a ParkingLot (so the ownership of a self pointer doesn't really have
     * implications for copy/move, as the objects shouldn't be moved/copied while waiting)
     */
    std::list<Notifiable*> _handleContainer{this};
};

/**
 * A ParkingLot allows Notifiables to wait for more than one notification signal. By "parking" a
 * Notifiable through ParkingLot::parkOne, the Notifiable will wait using the provided callback
 * until _either_ `notify` is called on the Notifiable, _or_ the Notifiable is unparked (via
 * ParkingLot::unparkOne or ParkingLot::unparkAll) from the ParkingLot.
 *
 * A motivating use case is an Notifiable waiting for some condition to change _or_ an interruption.
 * If the condition changes, a ParkingLot can notify _any_ Notifiable interested in the condition by
 * unparking a Notifiable parked in a ParkingLot associated with the condition. If a particular
 * Notifiable is interrupted, via a call to `notify` on that Notifiable, than that particular
 * Notifiable will wake from its wait and be removed from the ParkingLot.
 */
class ParkingLot {
public:
    /**
     * Unpark one Notifiable from the ParkingLot. Returns true if something was unparked, otherwise
     * false. Notifiables are dequeued in FIFO order.
     */
    bool unparkOne() noexcept {
        if (_notifiableCount.load()) {
            stdx::lock_guard lk(_mutex);

            return _notifyNextNotifiable(lk);
        }
        return false;
    }

    /** Unpark everything currently parked. */
    void unparkAll() noexcept {
        if (_notifiableCount.load()) {
            stdx::lock_guard lk(_mutex);
            while (_notifyNextNotifiable(lk)) {
            }
        }
    }


    /**
     * Park `notifiable` and run `cb `. This ensures that for the
     * duration of the callback execution, unparking the notifiable will trigger a notify() to
     * the Notifiable. Note that it is possible for the notifiable to be notified _independently_
     * of the parking lot, i.e. via some mechanism _other_ than the unpark{One,All} functions on
     * the parking lot.
     *
     * The scheme here is that list entries are spliced back to their Notifiable from the
     * notification list when notified (so that they don't eat multiple unparkOne's).  We detect
     * that condition by noting that our list isn't empty (in which case we should avoid a double
     * splice back).
     */
    template <typename Callback>
    void parkOne(Notifiable& notifiable, Callback&& cb) noexcept {
        static_assert(noexcept(std::forward<Callback>(cb)()),
                      "Only noexcept functions may be invoked with parkOne");
        // A notifiable's entry in the waiter list is spliced from itself into the waiter list
        // here, under the mutex, before we begin waiting. When the notifiable is notified
        // independently of the parking lot, itsplices itself out of the waiter-list back into its
        // _handleContainer. This ensures it won't consume any unparks. When the notifiable is
        // notified via unparking, the unparking thread splices it out of the waiter list into the
        // notifiable's _handleContainer under the lock, to ensure a single notifiable can't consume
        // multiple unpark operations. _handleContainer is manipulated only when _mutex is held, so
        // we can check upon wake-up if the _handleContainer was already re-populated (in case we
        // were unparked), or if we need to spliceBack ourselves, under the lock.
        auto iter = [&] {
            stdx::lock_guard localMutex(_mutex);
            auto& notifiableHandleContainer = notifiable.getHandleContainer();
            _notifiableCount.addAndFetch(1);
            _notifiables.splice(
                _notifiables.end(), notifiableHandleContainer, notifiableHandleContainer.begin());
            return --_notifiables.end();
        }();

        // `cb()` should wait until `notify` is called on `notifiable`.
        // as well as any other work the waiter would like to do while waiting.
        std::forward<Callback>(cb)();

        stdx::lock_guard localMutex(_mutex);
        // If our list isn't empty, we were notified, and spliced back in _notifyNextNotifiable.
        // If it is empty, we need to stash our wait queue iterator ourselves.
        if (notifiable.getHandleContainer().empty()) {
            _notifiableCount.subtractAndFetch(1);
            _spliceBack(localMutex, iter);
        }
    }

private:
    /**
     * Notifies the next notifiable.
     *
     * Returns true if there was a notifiable notified.
     *
     * Note that as part of notifying, we splice back iterators to parkNotifiable callers.  This
     * is safe because we hold _mutex while we do so, and our splicing communicates that those
     * waiters need not clear themselves from the notification list on wakeup.
     */
    bool _notifyNextNotifiable(WithLock wl) noexcept {
        auto iter = _notifiables.begin();
        if (iter == _notifiables.end()) {
            return false;
        }

        (*iter)->notify();

        _spliceBack(wl, iter);

        return true;
    }

    /**
     * Splice the notifiable iterator back into the notifiable (out from this ParkingLot's wait
     * list).
     */
    void _spliceBack(WithLock, std::list<Notifiable*>::iterator iter) {
        auto notifiable = *iter;
        auto& notifiableHandleContainer = notifiable->getHandleContainer();
        notifiableHandleContainer.splice(notifiableHandleContainer.begin(), _notifiables, iter);
    }

    Atomic<uint64_t> _notifiableCount{0};
    stdx::mutex _mutex;
    std::list<Notifiable*> _notifiables;
};
}  // namespace mongo
