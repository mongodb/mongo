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

#include "mongo/db/operation_context.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/hierarchical_acquisition.h"
#include "mongo/util/time_support.h"

#include <boost/optional.hpp>

namespace mongo {

/**
 * Allows waiting for a result returned from an asynchronous operation.
 */
template <class T>
class Notification {
public:
    Notification() = default;

    /**
     * Creates a notification object, which has already been set. Calls to any of the getters will
     * return immediately.
     */
    explicit Notification(T value) : _value(value) {}

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
    T& get(OperationContext* opCtx) {
        stdx::unique_lock<stdx::mutex> lock(_mutex);
        opCtx->waitForConditionOrInterrupt(_condVar, lock, [this]() -> bool { return !!_value; });
        return _value.get();
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
     * If the notification is set, returns immediately. Otherwise, blocks until it either becomes
     * set or the waitTimeout expires, whichever comes first. Returns true if the notification is
     * set (in which case a subsequent call to get is guaranteed to not block) or false otherwise.
     * If the wait is interrupted, throws an exception.
     */
    bool waitFor(OperationContext* opCtx, Milliseconds waitTimeout) {
        stdx::unique_lock<stdx::mutex> lock(_mutex);
        return opCtx->waitForConditionOrInterruptFor(
            _condVar, lock, waitTimeout, [&]() { return !!_value; });
    }

private:
    mutable stdx::mutex _mutex;
    stdx::condition_variable _condVar;

    // Protected by mutex and only moves from not-set to set once
    boost::optional<T> _value{boost::none};
};

template <>
class Notification<void> {
public:
    explicit operator bool() const {
        return _notification.operator bool();
    }

    void get(OperationContext* opCtx) {
        _notification.get(opCtx);
    }

    void get() {
        _notification.get();
    }

    void set() {
        _notification.set(true);
    }

    bool waitFor(OperationContext* opCtx, Milliseconds waitTimeout) {
        return _notification.waitFor(opCtx, waitTimeout);
    }

private:
    Notification<bool> _notification;
};

}  // namespace mongo
