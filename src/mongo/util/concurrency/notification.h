/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <boost/optional.hpp>

#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

namespace mongo {

class OperationContext;

/**
 * Allows waiting for a result returned from an asynchronous operation.
 */
template <class T>
class Notification {
public:
    /**
     * Returns true if the notification has been set (i.e., the call to get/waitFor would not
     * block).
     */
    explicit operator bool() const {
        stdx::unique_lock<stdx::mutex> lock(_mutex);
        return !!_value;
    }

    /**
     * If the notification has been set, returns immediately. Otherwise blocks until it becomes set.
     * If the wait is interrupted, throws an exception.
     */
    T& get(OperationContext* txn) {
        return get();
    }

    /**
     * If the notification has been set, returns immediately. Otherwise blocks until it becomes set.
     * This variant of get cannot be interrupted.
     */
    T& get() {
        stdx::unique_lock<stdx::mutex> lock(_mutex);
        while (!_value) {
            _condVar.wait(lock);
        }

        return _value.get();
    }

    /**
     * Sets the notification result and wakes up any threads, which might be blocked in the wait
     * call. Must only be called once for the lifetime of the notification.
     */
    void set(T value) {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        invariant(!_value);
        _value = std::move(value);
        _condVar.notify_all();
    }

    /**
     * If the notification is not set, blocks either until it becomes set or until the waitTimeout
     * expires. If the wait is interrupted, throws an exception. Otherwise, returns immediately.
     */
    bool waitFor(OperationContext* txn, Microseconds waitTimeout) {
        const auto waitDeadline = Date_t::now() + waitTimeout;

        stdx::unique_lock<stdx::mutex> lock(_mutex);
        return _condVar.wait_until(
            lock, waitDeadline.toSystemTimePoint(), [&]() { return !!_value; });
    }

private:
    mutable stdx::mutex _mutex;
    mutable stdx::condition_variable _condVar;

    // Protected by mutex and only moves from not-set to set once
    boost::optional<T> _value{boost::none};
};

template <>
class Notification<void> {
public:
    explicit operator bool() const {
        return _notification.operator bool();
    }

    void get(OperationContext* txn) {
        _notification.get(txn);
    }

    void get() {
        _notification.get();
    }

    void set() {
        _notification.set(true);
    }

    bool waitFor(OperationContext* txn, Microseconds waitTimeout) {
        return _notification.waitFor(txn, waitTimeout);
    }

private:
    Notification<bool> _notification;
};

}  // namespace mongo
