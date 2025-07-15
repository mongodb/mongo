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

#include "mongo/platform/mutex.h"
#include "mongo/platform/waitable_atomic.h"

namespace mongo {

/**
 * A shared mutex type optimized for readers, with the assumption of infrequent writes. Under the
 * hood, it is very similar to a hazard pointer, where each thread maintains a list for its shared
 * lock acquisitions. Writers must scan these lists and block until all shared locks are released.
 * The primary advantage of this over existing synchronization types is that in absence of writes,
 * the cost of acquiring shared locks is constant, regardless of the number of CPU cores/sockets.
 */
class alignas(64) WriteRarelyRWMutex {
public:
    template <bool LockExclusively>
    class [[nodiscard]] ScopedLock {
    public:
        explicit ScopedLock(WriteRarelyRWMutex* rwMutex) : _rwMutex(rwMutex) {
            if constexpr (LockExclusively) {
                _rwMutex->_lock();
            } else {
                _rwMutex->_lock_shared();
            }
        }

        ~ScopedLock() {
            if (_rwMutex) {
                if constexpr (LockExclusively) {
                    _rwMutex->_unlock();
                } else {
                    _rwMutex->_unlock_shared();
                }
            }
        }

        ScopedLock(const ScopedLock&) = delete;
        ScopedLock& operator=(const ScopedLock&) = delete;

        ScopedLock(ScopedLock&& other) noexcept : _rwMutex(std::exchange(other._rwMutex, {})) {}

    private:
        ScopedLock() = default;
        WriteRarelyRWMutex* _rwMutex;
    };

    using ReadLock = ScopedLock<false>;
    using WriteLock = ScopedLock<true>;

    WriteRarelyRWMutex() = default;

    auto writeLock() noexcept {
        return WriteLock(this);
    }

    auto readLock() noexcept {
        return ReadLock(this);
    }

private:
    void _releaseSharedLockAndWaitForWriter() noexcept;

    void _lock() noexcept;
    void _unlock() noexcept;

    void _lock_shared() noexcept;
    void _unlock_shared() noexcept;

    friend bool isWriteFlagSet_forTest(const WriteRarelyRWMutex& mutex) {
        return mutex._writeFlag.load();
    }

    friend bool hasWaitersOnWriteFlag_forTest(const WriteRarelyRWMutex& mutex) {
        return hasWaiters_forTest(mutex._writeFlag);
    }

    Mutex _writeMutex;

    // Will be non-zero when a writer is either waiting for or is holding the lock. May only be
    // modified while holding `_writeMutex`.
    WaitableAtomic<int> _writeFlag{0};
};

namespace write_rarely_rwmutex_details {

/**
 * Test-only utility that clears the global state of the lock registry, and ensures there are no
 * active threads with thread-local lock lists.
 */
void resetGlobalLockRegistry_forTest();

}  // namespace write_rarely_rwmutex_details

}  // namespace mongo
