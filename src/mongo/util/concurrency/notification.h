// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <mutex>

#include <boost/optional.hpp>

namespace mongo {

/**
 * Allows waiting for a result returned from an asynchronous operation.
 */
template <class T>
class [[MONGO_MOD_USE_REPLACEMENT(Promise and Future)]] Notification {
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
        std::unique_lock<std::mutex> lock(_mutex);
        return !!_value;
    }

    /**
     * If the notification has been set, returns immediately. Otherwise blocks until it becomes set.
     * If the wait is interrupted, throws an exception.
     */
    T& get(OperationContext* opCtx) {
        std::unique_lock<std::mutex> lock(_mutex);
        opCtx->waitForConditionOrInterrupt(_condVar, lock, [this]() -> bool { return !!_value; });
        return _value.get();
    }

    /**
     * If the notification has been set, returns immediately. Otherwise blocks until it becomes set.
     * This variant of get cannot be interrupted.
     */
    T& get() {
        std::unique_lock<std::mutex> lock(_mutex);
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
        std::lock_guard<std::mutex> lock(_mutex);
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
        std::unique_lock<std::mutex> lock(_mutex);
        return opCtx->waitForConditionOrInterruptFor(
            _condVar, lock, waitTimeout, [&]() { return !!_value; });
    }

private:
    mutable std::mutex _mutex;
    stdx::condition_variable _condVar;

    // Protected by mutex and only moves from not-set to set once
    boost::optional<T> _value{boost::none};
};

template <>
class [[MONGO_MOD_USE_REPLACEMENT(Promise and Future)]] Notification<void> {
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
