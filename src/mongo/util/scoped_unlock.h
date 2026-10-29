// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <mutex>

[[MONGO_MOD_PUBLIC]];

namespace mongo {
/**
 * RAII object that unlocks a unique_lock type on construction, and relocks it on destruction. The
 * unique_lock must be locked when it is given to ScopedUnlock.
 */
template <typename T>
class ScopedUnlock {
public:
    /**
     * Construct a new Scoped Unlock object.
     * Unique_locks passed into this constructor must be locked, or an invariant failure will be
     * thrown.
     */
    explicit ScopedUnlock(std::unique_lock<T>& lock) : _lock(lock) {
        invariant(_lock.owns_lock(), "Locks in ScopedUnlock must be locked on initialization.");
        _lock.unlock();
    }

    ~ScopedUnlock() {
        if (!_dismissed) {
            _lock.lock();
        }
    }

    ScopedUnlock(const ScopedUnlock&) = delete;
    ScopedUnlock(ScopedUnlock&&) = delete;
    ScopedUnlock& operator=(const ScopedUnlock&) = delete;
    ScopedUnlock& operator=(ScopedUnlock&&) = delete;

    /** A dismissed ScopedUnlock does not lock on destruction. */
    void dismiss() noexcept {
        _dismissed = true;
    }

private:
    std::unique_lock<T>& _lock;
    bool _dismissed = false;
};

}  // namespace mongo
