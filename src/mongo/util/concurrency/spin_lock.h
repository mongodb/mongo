// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#ifdef _WIN32
#include "mongo/platform/windows_basic.h"
#else
#include <atomic>
#endif

#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/platform/compiler.h"
#include "mongo/util/modules.h"

#include <mutex>

namespace [[MONGO_MOD_PUBLIC]] mongo {

#if defined(_WIN32)
class SpinLock {
    SpinLock(const SpinLock&) = delete;
    SpinLock& operator=(const SpinLock&) = delete;

public:
    SpinLock() {
        InitializeCriticalSectionAndSpinCount(&_cs, 4000);
    }

    ~SpinLock() {
        DeleteCriticalSection(&_cs);
    }

    void lock() {
        EnterCriticalSection(&_cs);
    }

    void unlock() {
        LeaveCriticalSection(&_cs);
    }

    bool try_lock() {
        return TryEnterCriticalSection(&_cs);
    }

private:
    CRITICAL_SECTION _cs;
};

#else

#if MONGO_CONFIG_DEBUG_BUILD
class SpinLock {
    SpinLock(const SpinLock&) = delete;
    SpinLock& operator=(const SpinLock&) = delete;

public:
    SpinLock() = default;

    void lock() {
        _mutex.lock();
    }

    void unlock() {
        _mutex.unlock();
    }

    bool try_lock() {
        return _mutex.try_lock();
    }

private:
    std::mutex _mutex;
};

#else

class SpinLock {
    SpinLock(const SpinLock&) = delete;
    SpinLock& operator=(const SpinLock&) = delete;

public:
    SpinLock() = default;

    void unlock() {
        _locked.clear(std::memory_order_release);
    }

    void lock() {
        if (MONGO_likely(_tryLock()))
            return;
        _lockSlowPath();
    }

    bool try_lock() {
        return _tryLock();
    }

private:
    bool _tryLock() {
        bool wasLocked = _locked.test_and_set(std::memory_order_acquire);
        return !wasLocked;
    }

    void _lockSlowPath();

    // Initializes to the cleared state.
    std::atomic_flag _locked = ATOMIC_FLAG_INIT;  // NOLINT
};

#endif

#endif

using scoped_spinlock = std::lock_guard<SpinLock>;

}  // namespace mongo
