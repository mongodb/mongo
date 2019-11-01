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

#include "mongo/platform/mutex.h"

namespace mongo {

namespace details {
/**
 * Complimentary class to synchronized_value. This class is an implementation detail of
 * synchronized_value.
 *
 * Holds a lock and a reference to the value protected by synchronized_value.
 */
template <class T>
class update_guard {
public:
    /**
     * Take lock on construction to guard value.
     */
    explicit update_guard(T& value, Mutex& mtx) : _lock(mtx), _value(value) {}
    ~update_guard() = default;

    // Only move construction is permitted so that synchronized_value may return update_guard
    update_guard(update_guard&&) = default;
    update_guard& operator=(update_guard&&) = default;

    // Disallow any copy
    update_guard(update_guard const&) = delete;
    update_guard& operator=(update_guard const&) = delete;

    T& operator*() noexcept {
        return _value;
    }

    T* operator->() noexcept {
        return &_value;
    }

    update_guard& operator=(const T& value) {
        _value = value;
        return *this;
    }

    update_guard& operator=(T&& value) {
        _value = std::move(value);
        return *this;
    }

    operator T() const {
        return _value;
    }

private:
    // Held lock from synchronized_value
    stdx::unique_lock<Latch> _lock;

    // Reference to the value from synchronized_value
    T& _value;
};

/**
 * Const version of update_guard
 */
template <class T>
class const_update_guard {
public:
    /**
     * Take lock on construction to guard value.
     */
    explicit const_update_guard(const T& value, Mutex& mtx) : _lock(mtx), _value(value) {}
    ~const_update_guard() = default;

    // Only move construction is permitted so that synchronized_value may return const_update_guard
    const_update_guard(const_update_guard&&) = default;
    const_update_guard& operator=(const_update_guard&&) = default;

    // Disallow any copy
    const_update_guard(const_update_guard const&) = delete;
    const_update_guard& operator=(const_update_guard const&) = delete;

    T& operator*() const noexcept {
        return _value;
    }

    T* operator->() const noexcept {
        return &_value;
    }

    operator T() const {
        return _value;
    }

private:
    // Held lock from synchronized_value
    stdx::unique_lock<Latch> _lock;

    // Reference to the value from synchronized_value
    const T& _value;
};

}  // namespace details

/**
 * Provides mutex guarded access to an object.
 *
 * The protect object can be accessed by either:
 * 1. auto tmp = sv.synchronize()
 *    This is useful if you need to do multiple operators
 * 2. operator* or operator->
 *    Holds the lock for the duration of the call.
 * 3. get() - makes a copy of the object while the lock is held
 *
 * Inspired by https://isocpp.org/files/papers/n4033.html and boost::synchronized_value
 */
template <typename T>
class synchronized_value {
public:
    synchronized_value() = default;
    explicit synchronized_value(T value) : _value(std::move(value)) {}

    ~synchronized_value() = default;

    // Disallow copy and copy assignment
    synchronized_value(synchronized_value const&) = delete;
    synchronized_value& operator=(synchronized_value const&) = delete;

    // Support assigning from the contained value
    synchronized_value& operator=(const T& value) {
        {
            stdx::lock_guard<Latch> lock(_mutex);
            _value = value;
        }
        return *this;
    }

    synchronized_value& operator=(T&& value) {
        {
            stdx::lock_guard<Latch> lock(_mutex);
            _value = std::move(value);
        }
        return *this;
    }

    /**
     * Return a copy of the protected object.
     */
    T get() {
        stdx::lock_guard<Latch> lock(_mutex);
        return _value;
    }

    details::update_guard<T> operator->() {
        return details::update_guard<T>(_value, _mutex);
    }

    const details::const_update_guard<T> operator->() const {
        return details::update_guard<T>(_value, _mutex);
    }

    details::update_guard<T> operator*() {
        return details::update_guard<T>(_value, _mutex);
    }

    const details::const_update_guard<T> operator*() const {
        return details::const_update_guard<T>(_value, _mutex);
    }

    /**
     * Lock the synchronized_value and return a holder to the value and lock.
     */
    details::update_guard<T> synchronize() {
        return details::update_guard<T>(_value, _mutex);
    }

    bool operator==(synchronized_value const& rhs) const {
        // TODO: C++17 - move from std::lock to std::scoped_lock
        std::lock(_mutex, rhs._mutex);
        stdx::lock_guard<Latch> lk1(_mutex, stdx::adopt_lock);
        stdx::lock_guard<Latch> lk2(rhs._mutex, stdx::adopt_lock);
        return _value == rhs._value;
    }

    bool operator!=(synchronized_value const& rhs) const {
        // TODO: C++17 - move from std::lock to std::scoped_lock
        std::lock(_mutex, rhs._mutex);
        stdx::lock_guard<Latch> lk1(_mutex, stdx::adopt_lock);
        stdx::lock_guard<Latch> lk2(rhs._mutex, stdx::adopt_lock);
        return _value != rhs._value;
    }

    bool operator==(T const& rhs) const {
        stdx::lock_guard<Latch> lock1(_mutex);
        return _value == rhs;
    }

    bool operator!=(T const& rhs) const {
        stdx::lock_guard<Latch> lock1(_mutex);
        return _value != rhs;
    }

    template <class U>
    friend bool operator==(const synchronized_value<U>& lhs, const U& rhs);

    template <class U>
    friend bool operator!=(const synchronized_value<U>& lhs, const U& rhs);

    template <class U>
    friend bool operator==(const U& lhs, const synchronized_value<U>& rhs);

    template <class U>
    friend bool operator!=(const U& lhs, const synchronized_value<U>& rhs);

    template <class U>
    friend bool operator==(const synchronized_value<U>& lhs, const synchronized_value<U>& rhs);

    template <class U>
    friend bool operator!=(const synchronized_value<U>& lhs, const synchronized_value<U>& rhs);

private:
    // Value guarded by mutex
    T _value;

    // Mutex to guard value
    mutable Mutex _mutex = MONGO_MAKE_LATCH("synchronized_value::_mutex");
};

template <class T>
bool operator==(const synchronized_value<T>& lhs, const T& rhs) {
    stdx::lock_guard<Latch> lock(lhs._mutex);

    return lhs._value == rhs;
}

template <class T>
bool operator!=(const synchronized_value<T>& lhs, const T& rhs) {
    return !(lhs == rhs);
}

template <class T>
bool operator==(const T& lhs, const synchronized_value<T>& rhs) {
    stdx::lock_guard<Latch> lock(rhs._mutex);

    return lhs == rhs._value;
}

template <class T>
bool operator!=(const T& lhs, const synchronized_value<T>& rhs) {
    return !(lhs == rhs);
}

template <class T>
bool operator==(const synchronized_value<T>& lhs, const synchronized_value<T>& rhs) {
    // TODO: C++17 - move from std::lock to std::scoped_lock
    std::lock(lhs._mutex, rhs._mutex);
    stdx::lock_guard<Latch> lk1(lhs._mutex, stdx::adopt_lock);
    stdx::lock_guard<Latch> lk2(rhs._mutex, stdx::adopt_lock);

    return lhs._value == rhs._value;
}

template <class T>
bool operator!=(const synchronized_value<T>& lhs, const synchronized_value<T>& rhs) {
    return !(lhs == rhs);
}

}  // namespace mongo
