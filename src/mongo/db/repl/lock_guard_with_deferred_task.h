// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/functional.h"

#include <mutex>
#include <utility>

#include <absl/container/inlined_vector.h>

namespace mongo {
namespace repl {

/**
 * A lock guard that lets code running under the lock schedule work to run right after the lock is
 * released.
 *
 * Code deep under the ReplicationCoordinator mutex is handed proof that the mutex is held and
 * cannot release it, yet some of the work it triggers -- waking write-concern waiters, and the
 * continuations that run when their promises are fulfilled -- must not run under that mutex. Such a
 * function takes a `LockGuardWithDeferredTask&` in place of a `WithLock` and passes the work to
 * scheduleDeferredTask(). The guard's destructor releases the mutex and then runs the scheduled
 * tasks, in the order they were scheduled, on the same thread.
 *
 * Scheduled tasks are held in a buffer whose inline capacity covers the single task a call usually
 * schedules, so the common case does not allocate. The guard converts to `WithLock`, so it can
 * still be passed to anything that only needs proof the mutex is held, and it exposes
 * lock()/unlock() for call sites that release the mutex early.
 */
template <typename MutexType>
class LockGuardWithDeferredTask {
public:
    using DeferredTask = unique_function<void()>;

    explicit LockGuardWithDeferredTask(MutexType& mutex) : _lock(mutex) {}

    // Movable so that the lock -- and the responsibility for running the deferred tasks -- can be
    // handed to another scope. A moved-from guard does nothing when it is destroyed.
    LockGuardWithDeferredTask(LockGuardWithDeferredTask&& other) noexcept
        : _lock(std::move(other._lock)), _tasks(std::move(other._tasks)) {
        other._owns = false;
    }

    ~LockGuardWithDeferredTask() {
        if (!_owns) {
            return;
        }
        if (_lock.owns_lock()) {
            _lock.unlock();
        }
        // The mutex is released, so neither the task nor any continuation it runs inline contends
        // with it.
        for (auto& task : _tasks) {
            task();
        }
    }

    LockGuardWithDeferredTask(const LockGuardWithDeferredTask&) = delete;
    LockGuardWithDeferredTask& operator=(const LockGuardWithDeferredTask&) = delete;
    LockGuardWithDeferredTask& operator=(LockGuardWithDeferredTask&&) = delete;

    /**
     * Registers `task` to run once this guard releases the mutex. Tasks run in the order scheduled.
     * `task` must be callable -- scheduling an empty task is a programming error, not a no-op.
     */
    void scheduleDeferredTask(DeferredTask task) {
        iassert(ErrorCodes::IllegalOperation, "Deferred task must be callable", bool(task));
        _tasks.push_back(std::move(task));
    }

    operator WithLock() const {
        return WithLock(_lock);
    }

    bool owns_lock() const {
        return _lock.owns_lock();
    }

    void lock() {
        _lock.lock();
    }

    void unlock() {
        _lock.unlock();
    }

private:
    // A call schedules a single deferred task in the common case; more than that spills to the
    // heap.
    using TaskBuffer = absl::InlinedVector<DeferredTask, 1>;

    std::unique_lock<MutexType> _lock;

    // The tasks scheduled under this guard, run in order once the mutex is released.
    TaskBuffer _tasks;

    // False once this guard has been moved from: the guard it was moved into owns the lock and the
    // deferred tasks now.
    bool _owns = true;
};

}  // namespace repl
}  // namespace mongo
