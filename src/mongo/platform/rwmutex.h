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

#include "mongo/platform/compiler.h"
#include "mongo/platform/waitable_atomic.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

namespace MONGO_MOD_PUB mongo {

/**
 * A reader-writer mutex type that is optimized for frequent, short reads and infrequent writes.
 * This type is not fair towards readers, as back-to-back writes may starve reads. Therefore, this
 * type is not suitable for use-cases where the mutex is acquired in exclusive mode in a tight loop.
 *
 * Note that `RWMutex` is not interruptible and provides similar semantics to `std::shared_mutex`.
 * Make sure to closely examine your code before using `RWMutex` over `Mutex` and verify that the
 * synchronization pattern is a good match for `RWMutex`.
 */
class RWMutex {
public:
    using StateType MONGO_MOD_FILE_PRIVATE = uint32_t;
    MONGO_MOD_FILE_PRIVATE static constexpr StateType kWriteIntentMask = 1 << 31;
    MONGO_MOD_FILE_PRIVATE static constexpr StateType kReadersCountMask = ~kWriteIntentMask;
    MONGO_MOD_FILE_PRIVATE static constexpr StateType kReadersOverflowMask = 1 << 30;

    void lock() {
        _writeMutex.lock();
        auto state = _state.fetchAndBitOr(kWriteIntentMask) | kWriteIntentMask;
        while (state & kReadersCountMask) {
            // Keep waiting here until there are no readers. Any new reader will notice the write
            // intent and withdraw.
            state = _state.wait(state);
        }
    }

    void unlock() {
        _state.fetchAndBitXor(kWriteIntentMask);
        _state.notifyAll();
        _writeMutex.unlock();
    }

    void lock_shared() {
        if (auto state = _state.addAndFetch(1);
            MONGO_unlikely(_hasPendingWriterOrTooManyReaders(state))) {
            // A write is in progress. Clear the read intent and wait until we can lock for reading.
            _waitAndThenLock(state);
        }
    }

    void unlock_shared() {
        if (MONGO_unlikely(_state.subtractAndFetch(1) == kWriteIntentMask)) {
            // A writer is waiting and this is the last reader, so we need to notify the waiters.
            _state.notifyAll();
        }
    }

private:
    friend void setWriteIntent_forTest(RWMutex& mutex) {
        mutex._state.fetchAndBitOr(kWriteIntentMask);
    }

    friend bool isWriteIntentSet_forTest(const RWMutex& mutex) {
        return mutex._state.load() & kWriteIntentMask;
    }

    friend void addReaders_forTest(RWMutex& mutex, uint32_t readers) {
        mutex._state.fetchAndAdd(readers);
    }

    friend bool hasWaiters_forTest(const RWMutex& mutex) {
        return hasWaiters_forTest(mutex._state);
    }

    friend size_t getReadersCount_forTest(const RWMutex& mutex) {
        return mutex._state.load() & kReadersCountMask;
    }

    inline bool _hasPendingWriterOrTooManyReaders(StateType state) const {
        return state & (kWriteIntentMask | kReadersOverflowMask);
    }

    MONGO_COMPILER_NOINLINE MONGO_COMPILER_COLD_FUNCTION void _waitAndThenLock(StateType state) {
        do {
            invariant(!(state & kReadersOverflowMask), "Too many readers have acquired the lock!");
            unlock_shared();
            while (state & kWriteIntentMask) {
                // Wait here until the write intent is cleared.
                state = _state.wait(state);
            }
            state = _state.addAndFetch(1);
        } while (MONGO_unlikely(_hasPendingWriterOrTooManyReaders(state)));
    }

    // Synchronizes writers, only allowing a single writer to acquire the mutex at any time.
    stdx::mutex _writeMutex;

    /**
     * Bits [0 .. 29] represent the number of readers, allowing up to 2 ^ 30 - 1 concurrent reads.
     * Bit 30 must remain zero and allows preventing too many readers.
     * Bit 31 tracks the write intent.
     */
    WaitableAtomic<StateType> _state{0};
};

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

    auto writeLock() {
        return WriteLock(this);
    }

    auto readLock() {
        return ReadLock(this);
    }

private:
    void _releaseSharedLockAndWaitForWriter();

    void _lock();
    void _unlock();

    void _lock_shared();
    void _unlock_shared();

    friend bool isWriteFlagSet_forTest(const WriteRarelyRWMutex& mutex) {
        return mutex._writeFlag.load();
    }

    friend bool hasWaitersOnWriteFlag_forTest(const WriteRarelyRWMutex& mutex) {
        return hasWaiters_forTest(mutex._writeFlag);
    }

    stdx::mutex _writeMutex;

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

}  // namespace MONGO_MOD_PUB mongo
