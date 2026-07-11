// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/repl/clang_checked/lockable_concepts.h"
#include "mongo/db/repl/clang_checked/thread_safety_annotations.h"
#include "mongo/util/modules.h"

#include <concepts>

namespace [[MONGO_MOD_PUBLIC]] mongo {
namespace clang_checked {

/**
 * CheckedMutex is a wrapper for other mutex types for cases where clang's
 * thread safety checks are desirable. By combining CheckedMutex and the
 * annotations defined in thread_safety_annotations.h in this directory, you
 * can enforce constraints such as "this member is always guarded by that
 * mutex."
 */
template <BaseLockable MutexT>
class MONGO_LOCKING_CAPABILITY("mutex") CheckedMutex {
public:
    using mutex_type = MutexT;

    void lock() MONGO_LOCKING_ACQUIRE() {
        _m.lock();
    }

    bool try_lock()
    requires TryLockable<mutex_type>
    MONGO_LOCKING_TRY_ACQUIRE(true) {
        return _m.try_lock();
    }

    void unlock() MONGO_LOCKING_RELEASE() {
        _m.unlock();
    }

private:
    mutable mutex_type _m;
};

}  // namespace clang_checked
}  // namespace mongo
