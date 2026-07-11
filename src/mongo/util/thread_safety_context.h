// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/platform/atomic.h"
#include "mongo/util/modules.h"

[[MONGO_MOD_PUBLIC]];

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
    Atomic<bool> _isSingleThreaded{true};

    // If set to "false", it will prevent creation of new threads.
    Atomic<bool> _safeToCreateThreads{true};
};

}  // namespace mongo
