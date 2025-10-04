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

#ifdef _WIN32
#include "mongo/platform/windows_basic.h"
#else
#include <atomic>
#endif

#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/platform/compiler.h"
#include "mongo/stdx/mutex.h"

#include <mutex>

namespace mongo {

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
    stdx::mutex _mutex;
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

using scoped_spinlock = stdx::lock_guard<SpinLock>;

}  // namespace mongo
