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

#include "mongo/platform/waitable_atomic.h"

#include "mongo/util/errno_util.h"

#ifdef __linux__
#include <linux/futex.h>
#include <sys/syscall.h>
#elif defined(_WIN32)
#include <synchapi.h>
#endif

namespace mongo::waitable_atomic_details {
using stdx::chrono::system_clock;

#ifdef __linux__

namespace {
// Note: these functions all use FUTEX_PRIVATE_FLAG because we never share futex addresses across
// processes or play tricks with multiply mapped mmap regions. This lets it take a faster path in
// the kernel.

void futexWake(const void* uaddr, int nToWake) {
    invariant(uaddr != nullptr);

    int futexWakeRet = syscall(SYS_futex,  //
                               uaddr,
                               FUTEX_WAKE | FUTEX_PRIVATE_FLAG,
                               nToWake);
    if (futexWakeRet == -1) {
        auto futexWakeError = lastSystemError();
        switch (futexWakeError.value()) {
            // These two are ignored, because there are some use cases where it is valid for an
            // object to be destroyed concurrently with it being notified.
            case EFAULT:
            case EACCES:
                return;

            default:
                invariant(futexWakeRet != -1, errorMessage(futexWakeError));
        }
    }
}

int futexWait(const void* uaddr, uint32_t val, boost::optional<system_clock::time_point> deadline) {
    invariant(uaddr != nullptr);

    timespec* tsAddr = nullptr;
    auto deadlineSpec = timespec{};
    if (deadline) {
        deadlineSpec.tv_sec = durationCount<Seconds>(deadline->time_since_epoch());
        deadlineSpec.tv_nsec = durationCount<Nanoseconds>(
            deadline->time_since_epoch() - stdx::chrono::seconds(deadlineSpec.tv_sec));
        tsAddr = &deadlineSpec;
    }

    // Using FUTEX_WAIT_BITSET (arg3) with FUTEX_BITSET_MATCH_ANY (last arg)
    // to achieve the equivalent of FUTEX_WAIT with an absolute timeout.
    return syscall(SYS_futex,
                   uaddr,
                   FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME,
                   val,
                   tsAddr,
                   (void*)nullptr,
                   FUTEX_BITSET_MATCH_ANY);
}
}  // namespace

void notifyOne(const void* uaddr) {
    futexWake(uaddr, 1);
}

void notifyMany(const void* uaddr, int nToWake) {
    futexWake(uaddr, nToWake);
}

void notifyAll(const void* uaddr) {
    futexWake(uaddr, INT_MAX);
}

bool waitUntil(const void* uaddr,
               uint32_t old,
               boost::optional<system_clock::time_point> deadline) {
    if (int futexWaitRet = futexWait(uaddr, old, deadline); futexWaitRet != 0) {
        auto futexWaitError = lastSystemError();
        switch (futexWaitError.value()) {
            case EAGAIN:  // old != *uaddr prior to waiting.
            case EINTR:   // Woken by a signal.
                break;

            case ETIMEDOUT:
                return false;

            default:
                invariant(futexWaitRet == 0, errorMessage(futexWaitError));
        }
    }
    return true;
}

#elif defined(_WIN32)

void notifyOne(const void* uaddr) {
    WakeByAddressSingle(const_cast<void*>(uaddr));
}

void notifyMany(const void* uaddr, int nToWake) {
    for (int i = 0; i < nToWake; ++i) {
        WakeByAddressSingle(const_cast<void*>(uaddr));
    }
}

void notifyAll(const void* uaddr) {
    WakeByAddressAll(const_cast<void*>(uaddr));
}

bool waitUntil(const void* uaddr,
               uint32_t old,
               boost::optional<system_clock::time_point> deadline) {
    DWORD timeout = INFINITE;  // No timeout
    bool timeoutOverflow = false;
    if (deadline) {
        int64_t millis = durationCount<Milliseconds>(*deadline - system_clock::now());
        if (millis <= 0)
            return false;  // Synthesize a timeout.

        if (millis >= int64_t(INFINITE)) {  // INFINITE is max uint32, not 0.
            // 2**32 micros is a little under 50 days. If this happens, wait as long as we can, then
            // return as-if a spurious wakeup happened, rather than a timeout. This will cause
            // the caller to loop and we will compute a smaller time each pass, eventually reaching
            // a representable timeout.
            millis = uint32_t(INFINITE) - 1;
            timeoutOverflow = true;
        }

        timeout = millis;
    }

    if (WaitOnAddress(const_cast<void*>(uaddr), &old, sizeof(old), timeout))
        return true;

    // There isn't a good list of possible errors, so assuming that anything other than a timeout
    // error is a possible spurious wakeup.
    return timeoutOverflow || GetLastError() != ERROR_TIMEOUT;
}

#elif defined(__APPLE__)

// These are unfortunately undocumented and not in any header. Basing implementation on libc++:
// https://github.com/llvm/llvm-project/blob/1482106c9960300e729b1a58e5e25b6ac1c150ba/libcxx/src/atomic.cpp#L45-L118
// There is also some useful info at https://outerproduct.net/futex-dictionary.html#macos

// Note that the declared type of value is misleading. With UL_COMPARE_AND_SWAP, only 4 bytes of
// addr will be examined:
// https://github.com/apple/darwin-xnu/blob/2ff845c2e033bd0ff64b5b6aa6063a1f8f65aa32/bsd/kern/sys_ulock.c#L503-L514
extern "C" int __ulock_wait(uint32_t operation,
                            void* addr,
                            uint64_t value,
                            uint32_t timeout); /* timeout is specified in microseconds */
extern "C" int __ulock_wake(uint32_t operation, void* addr, uint64_t wake_value);

#define UL_COMPARE_AND_WAIT 1
#define ULF_WAKE_ALL 0x00000100

void notifyOne(const void* uaddr) {
    __ulock_wake(UL_COMPARE_AND_WAIT, const_cast<void*>(uaddr), 0);
}

void notifyMany(const void* uaddr, int nToWake) {
    for (int i = 0; i < nToWake; ++i) {
        __ulock_wake(UL_COMPARE_AND_WAIT, const_cast<void*>(uaddr), 0);
    }
}

void notifyAll(const void* uaddr) {
    __ulock_wake(UL_COMPARE_AND_WAIT | ULF_WAKE_ALL, const_cast<void*>(uaddr), 0);
}

bool waitUntil(const void* uaddr,
               uint32_t old,
               boost::optional<system_clock::time_point> deadline) {
    uint32_t timeoutMicros = 0;  // 0 means wait forever, not don't wait at all!
    bool timeoutOverflow = false;
    if (deadline) {
        int64_t micros = durationCount<Microseconds>(*deadline - system_clock::now());
        if (micros <= 0) {
            return false;  // Synthesize a timeout.
        }

        if (micros > int64_t(std::numeric_limits<uint32_t>::max())) {
            // 2**32 micros is a little over an hour. If this happens, we wait as long as we can,
            // then return as-if a spurious wakeup happened, rather than a timeout. This will cause
            // the caller to loop and we will compute a smaller time each pass, eventually reaching
            // a representable timeout.
            micros = std::numeric_limits<uint32_t>::max();
            timeoutOverflow = true;
        }

        timeoutMicros = micros;
    }

    if (__ulock_wait(UL_COMPARE_AND_WAIT, const_cast<void*>(uaddr), old, timeoutMicros) != -1)
        return true;

    // There isn't a good list of possible errors, so assuming that anything other than a timeout
    // error is a possible spurious wakeup.
    return timeoutOverflow || errno != ETIMEDOUT;
}

#else
#error "Need an implementation of waitUntil(), notifyOne(), notifyMany(), notifyAll() for this OS"
#endif

}  // namespace mongo::waitable_atomic_details
