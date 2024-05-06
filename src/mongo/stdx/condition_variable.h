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

#include <condition_variable>

#include "mongo/util/notifyable.h"

namespace mongo {

class Waitable;

namespace stdx {

using cv_status = ::std::cv_status;      // NOLINT
using ::std::notify_all_at_thread_exit;  // NOLINT

/**
 * We wrap std::condition_variable_any to allow us to register Notifyables which can "wait" on the
 * condvar without actually waiting on the std::condition_variable_any. This allows us to possibly
 * do productive work in those types, rather than sleeping in the os.
 */
class condition_variable : private std::condition_variable_any {  // NOLINT
public:
    using std::condition_variable_any::condition_variable_any;  // NOLINT

    void notify_one() noexcept {
        if (!_parkingLot.notifyOne()) {
            std::condition_variable_any::notify_one();  // NOLINT
        }
    }

    void notify_all() noexcept {
        _parkingLot.notifyAll();
        std::condition_variable_any::notify_all();  // NOLINT
    }

    using std::condition_variable_any::wait;        // NOLINT
    using std::condition_variable_any::wait_for;    // NOLINT
    using std::condition_variable_any::wait_until;  // NOLINT

private:
    friend class ::mongo::Waitable;

    template <typename Callback>
    void _runWithNotifyable(Notifyable& notifyable, Callback&& cb) noexcept {
        _parkingLot.runWithNotifyable(notifyable, std::forward<Callback>(cb));
    }

    NotifyableParkingLot _parkingLot;
};

using condition_variable_any = stdx::condition_variable;
}  // namespace stdx
}  // namespace mongo
