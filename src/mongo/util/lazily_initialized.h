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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

#include "mongo/platform/atomic_word.h"

namespace mongo {
/**
 * This class acts a "holder" for a lazily-initialized object on the heap.
 *
 * A default-constructed LazilyInitialized<T> starts out in the "not initialized" state. When get()
 * is invoked, lazy initialization will occur if the LazilyInitialized<T> hasn't been initialized
 * yet.
 *
 * When a LazilyInitialized<T> is in the "initialized" state (i.e. '_ptr' is non-null), it owns the
 * object that '_ptr' points to, and it will free this object when LazilyInitialized<T>'s destructor
 * runs.
 *
 * If a LazilyInitialized<T> isn't initialized yet and multiple threads call get() concurrently,
 * it's possible that multiple threads will try to initialize the LazilyInitialized<T> at the same
 * time. In this scenario, each thread will attempt to set '_ptr' using atomic compare-and-swap.
 * One thread will succeed, setting '_ptr' to point to the object it created. All other threads'
 * attempts to set '_ptr' will fail, and these other threads will then discard the objects that they
 * created and just use whatever object '_ptr' was set to point to.
 *
 * Once a LazilyInitialized<T> is initialized and '_ptr' points to a given object, it's not possible
 * to set '_ptr' to null or to change it to point to a different object.
 */
template <typename T>
class LazilyInitialized {
public:
    static_assert(!std::is_array_v<T>);
    static_assert(!std::is_function_v<T>);

    // Default constructor will construct a LazilyInitialized<T> in the "uninitialized" state.
    LazilyInitialized() = default;

    // This constructor will construct a LazilyInitialized<T> in an initialized state with
    // '_ptr' pointing to the object held by 'p'.
    LazilyInitialized(std::unique_ptr<T> p) : _ptr(p.release()) {}

    LazilyInitialized(const LazilyInitialized&) = delete;
    LazilyInitialized(LazilyInitialized&&) = delete;

    ~LazilyInitialized() {
        if (T* ptr = _ptr.load()) {
            delete ptr;
        }
    }

    LazilyInitialized& operator=(const LazilyInitialized&) = delete;
    LazilyInitialized& operator=(LazilyInitialized&&) = delete;

    template <typename FuncT>
    T& get(const FuncT& initFn) {
        return getImpl(initFn);
    }

    template <typename FuncT>
    const T& get(const FuncT& initFn) const {
        return getImpl(initFn);
    }

    T* getIfInitialized() {
        return _ptr.load();
    }

    const T* getIfInitialized() const {
        return _ptr.load();
    }

private:
    template <typename FuncT>
    T& getImpl(const FuncT& initFn) const {
        // If '_ptr' is not null, return '*_ptr'.
        if (T* ptr = _ptr.load()) {
            return *ptr;
        }

        // Call initFn().
        std::unique_ptr<T> initPtr = std::unique_ptr<T>{initFn()};

        for (;;) {
            // Try to store the result of initFn() into '_ptr'.
            T* nullPtr = nullptr;
            if (_ptr.compareAndSwap(&nullPtr, initPtr.get())) {
                // If we succeeded, then call release() (to prevent the result of initFn() from
                // being freed) and return the pointer value.
                return *initPtr.release();
            }
            // If we failed to store the result of initFn(), check '_ptr' again. If '_ptr' is not
            // null, return the value of '*_ptr' (and allow the result of initFn() to be freed).
            if (T* ptr = _ptr.load()) {
                return *ptr;
            }
        }
    }

    mutable AtomicWord<T*> _ptr{nullptr};
};
}  // namespace mongo
