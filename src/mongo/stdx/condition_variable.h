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
#include "mongo/util/parking_lot.h"

namespace mongo {

namespace stdx {

using cv_status = ::std::cv_status;      // NOLINT
using ::std::notify_all_at_thread_exit;  // NOLINT

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
