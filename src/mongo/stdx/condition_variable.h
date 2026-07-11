// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/atomic.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/modules.h"
#include "mongo/util/parking_lot.h"

#include <atomic>
#include <condition_variable>
#include <list>
#include <mutex>

namespace [[MONGO_MOD_PUBLIC]] mongo {

namespace stdx {

using cv_status = ::std::cv_status;  // NOLINT

/**
 * We wrap std::condition_variable_any to allow us to register Notifiables which can "wait" on the
 * condvar without actually waiting on the std::condition_variable_any. This allows us to possibly
 * do productive work in those types, rather than sleeping in the os.
 */
class condition_variable : private std::condition_variable_any {  // NOLINT
public:
    using std::condition_variable_any::condition_variable_any;  // NOLINT

    /**
     * Use this function instead of wait/wait_for/wait_until to wait using `notifiable`.
     *
     * Waiting using `notifiable` means that:
     *  1) `cb` will be called to perform the wait, instead of calling
     *  std::condition_variable::wait. `cb` should return when `notify` is called on `notifiable`.
     *  2) The wait may be stopped because this condvar is notified, _or_ because `notifiable` was
     *  notified for some other reason (i.e. interrupt)
     *  3) Other work may be performed by the waiting thread before it yields; the logic for
     *  performing this work is encapsulated inside of `cb`; the only constraint is that it returns
     *  once notification has been performed on `notifiable`. Note that `notifiable` notificaton is
     *  a one-time event; once it is notified, all calls to this function will return immediately
     *  without waiting. See `Notifiable` and `ParkingLot` for more.
     *
     *  For example, passing `[baton] () { return baton->run(); }` as `cb` would be suitable when
     *  `baton` is also passed as `notifiable`, becuase `Baton::run` will return when the baton is
     *  notified.
     */
    template <typename Callback>
    void waitWithNotifiable(Notifiable& notifiable, Callback&& cb) noexcept {
        return _parkingLot.parkOne(notifiable, std::forward<Callback>(cb));
    }

    void notify_one() noexcept {
        if (!_parkingLot.unparkOne()) {
            std::condition_variable_any::notify_one();  // NOLINT
        }
    }

    void notify_all() noexcept {
        _parkingLot.unparkAll();
        std::condition_variable_any::notify_all();  // NOLINT
    }

    using std::condition_variable_any::wait;        // NOLINT
    using std::condition_variable_any::wait_for;    // NOLINT
    using std::condition_variable_any::wait_until;  // NOLINT

private:
    ParkingLot _parkingLot;
};

using condition_variable_any = stdx::condition_variable;
}  // namespace stdx
}  // namespace mongo
