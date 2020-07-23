/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/atomic_word.h"

namespace mongo {

class ThreadSafetyContextTest;

/**
 * Provides a singleton object that allows enforcing a single-threaded context at startup.
 *
 * This class, currently, intercepts `stdx::thread` creations and aborts the process if a single
 * threaded context is expected.
 *
 * You should avoid using the interfaces that specify single-threaded context (i.e.,
 * `forbidMultiThreading()` and `allowMultiThreading()`) outside the main function.
 *
 * If using other APIs for spawning threads (e.g., `pthread_create()`), make sure to precede the
 * API call with `onThreadCreate()`.
 */
class ThreadSafetyContext final {
public:
    ThreadSafetyContext(const ThreadSafetyContext&) = delete;

    ThreadSafetyContext(ThreadSafetyContext&&) noexcept = delete;

    ThreadSafetyContext& operator=(const ThreadSafetyContext&) = delete;

    ThreadSafetyContext& operator=(ThreadSafetyContext&&) = delete;

    static ThreadSafetyContext* getThreadSafetyContext() noexcept;

    // Prevents a multi-threaded context -- aborts the process on thread creation.
    // If the program is already multi-threaded, it will abort the program.
    void forbidMultiThreading() noexcept;

    // Allows a multi-threaded context by lifting the limit enforced by `forbidMultiThreading()`.
    void allowMultiThreading() noexcept;

    // Allows detecting thread creation, and thus a multi-threaded context.
    // If not using `stdx::thread`, you must always call this method before spawning a new thread.
    void onThreadCreate() noexcept;

    // Returns "true" if no threads have been created throughout the lifetime of the process.
    bool isSingleThreaded() const noexcept {
        return _isSingleThreaded.load();
    }

    // Restricts the ability of resetting the context to the unit-test fixture.
    friend class ThreadSafetyContextTest;

private:
    // Prevents creating any instance of the object (other than the singleton).
    ThreadSafetyContext() = default;

    // An indicator on whether the program is single-threaded.
    AtomicWord<bool> _isSingleThreaded{true};

    // If set to "false", it will prevent creation of new threads.
    AtomicWord<bool> _safeToCreateThreads{true};
};

}  // namespace mongo
