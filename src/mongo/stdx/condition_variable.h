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

#include <atomic>
#include <condition_variable>
#include <list>

#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/with_lock.h"

namespace mongo {

namespace stdx {
class condition_variable;
}

/**
 * Notifyable is a slim type meant to allow integration of special kinds of waiters for
 * stdx::condition_variable.  Specifially, the notify() on this type will be called directly from
 * stdx::condition_varibale::notify_(one|all).
 *
 * See Waitable for the stdx::condition_variable integration.
 */
class Notifyable {
    friend class stdx::condition_variable;

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
    // Notifyable's own a list node which they splice into a stdx::condition_variable's wait list
    // when waiting on one.  It's important that we pre-allocate this entry on construction.
    //
    // Note that the notifyable** in this list is only used and meaningful while the notifyable
    // waits on a condition variable (so the ownership of a self pointer doesn't really have
    // implications for copy/move, as the objects shouldn't be moved/copied while waiting)
    std::list<Notifyable*> _handleContainer{this};
};

class Waitable;

namespace stdx {

using condition_variable_any = ::std::condition_variable_any;  // NOLINT
using cv_status = ::std::cv_status;                            // NOLINT
using ::std::notify_all_at_thread_exit;                        // NOLINT

/**
 * We wrap std::condition_variable to allow us to register Notifyables which can "wait" on the
 * condvar without actually waiting on the std::condition_variable.  This allows us to possibly do
 * productive work in those types, rather than sleeping in the os.
 */
class condition_variable : private std::condition_variable {  // NOLINT
public:
    using std::condition_variable::condition_variable;  // NOLINT

    void notify_one() noexcept {
        if (_notifyableCount.load()) {
            stdx::lock_guard lk(_mutex);

            if (_notifyNextNotifyable(lk)) {
                return;
            }
        }

        std::condition_variable::notify_one();  // NOLINT
    }

    void notify_all() noexcept {
        if (_notifyableCount.load()) {
            stdx::lock_guard lk(_mutex);

            while (_notifyNextNotifyable(lk)) {
            }
        }

        std::condition_variable::notify_all();  // NOLINT
    }

    using std::condition_variable::native_handle;  // NOLINT
    using std::condition_variable::wait;           // NOLINT
    using std::condition_variable::wait_for;       // NOLINT
    using std::condition_variable::wait_until;     // NOLINT

private:
    friend class ::mongo::Waitable;

    /**
     * Runs the callback with the Notifyable registered on the condvar.  This ensures that for the
     * duration of the callback execution, a notification on the condvar will trigger a notify() to
     * the Notifyable.
     *
     * The scheme here is that list entries are spliced back to their Notifyable from the
     * notification list when notified (so that they don't eat multiple notify_one's).  We detect
     * that condition by noting that our list isn't empty (in which case we should avoid a double
     * splice back).
     *
     * The method is private, and accessed via friendship in Waitable.
     */
    template <typename Callback>
    void _runWithNotifyable(Notifyable& notifyable, Callback&& cb) noexcept {
        static_assert(noexcept(std::forward<Callback>(cb)()),
                      "Only noexcept functions may be invoked with _runWithNotifyable");

        auto iter = [&] {
            stdx::lock_guard localMutex(_mutex);
            _notifyableCount.addAndFetch(1);
            _notifyables.splice(_notifyables.end(),
                                notifyable._handleContainer,
                                notifyable._handleContainer.begin());
            return --_notifyables.end();
        }();

        // The supplied callback should do the equivalent of waiting on this condition_variable
        // (i.e. return on notify), as well as any other work the waiter would like to do while
        // waiting.
        std::forward<Callback>(cb)();

        stdx::lock_guard localMutex(_mutex);

        // If our list isn't empty, we were notified, and spliced back in _notifyNextNotifyable.
        // If it is empty, we need to stash our wait queue iterator ourselves.
        if (notifyable._handleContainer.empty()) {
            _notifyableCount.subtractAndFetch(1);
            _spliceBack(localMutex, iter);
        }
    }

    /**
     * Notifies the next notifyable.
     *
     * Returns true if there was a notifyable to be notified.
     *
     * Note that as part of notifying, we splice back iterators to _runWithNotifyable callers.  This
     * is safe because we hold _mutex while we do so, and our splicing communicates that those
     * waiters need not clear themselves from the notification list on wakeup.
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
     * Splice the notifyable iterator back into the notifyable (out from this condvar's wait list)
     */
    void _spliceBack(WithLock, std::list<Notifyable*>::iterator iter) {
        auto notifyable = *iter;
        notifyable->_handleContainer.splice(
            notifyable->_handleContainer.begin(), _notifyables, iter);
    }

    AtomicWord<unsigned long long> _notifyableCount;

    stdx::mutex _mutex;
    std::list<Notifyable*> _notifyables;
};

}  // namespace stdx
}  // namespace mongo
