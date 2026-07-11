// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/platform/process_id.h"


// IWYU pragma: no_include "syscall.h"

#ifndef _WIN32
#include <pthread.h>  // IWYU pragma: keep
#endif

#if defined(__linux__)
#include <sys/syscall.h>  // IWYU pragma: keep
#include <sys/types.h>    // IWYU pragma: keep
#endif
#ifdef __FreeBSD__
#include <pthread_np.h>
#endif

#include "mongo/base/static_assert.h"
#include "mongo/util/assert_util.h"  // IWYU pragma: keep

#include <limits>
#include <sstream>  // IWYU pragma: keep

namespace mongo {

MONGO_STATIC_ASSERT(sizeof(NativeProcessId) == sizeof(uint32_t));

namespace {
#ifdef _WIN32
inline NativeProcessId getCurrentNativeProcessId() {
    return GetCurrentProcessId();
}
#else
inline NativeProcessId getCurrentNativeProcessId() {
    return getpid();
}
#endif

#ifdef _WIN32
inline NativeProcessId getCurrentNativeThreadId() {
    return GetCurrentThreadId();
}
#elif __APPLE__
inline NativeProcessId getCurrentNativeThreadId() {
    // macOS deprecated syscall in 10.12.
    uint64_t tid;
    invariant(::pthread_threadid_np(NULL, &tid) == 0);
    return tid;
}
#elif __FreeBSD__
inline NativeProcessId getCurrentNativeThreadId() {
    return pthread_getthreadid_np();
}
#elif defined(__wasi__)
inline NativeProcessId getCurrentNativeThreadId() {
    // WASI doesn't support threads or thread IDs, so return a constant value
    // Using the SERVER-115422 ticket number as a return number for better debugging
    // and searchability in logs.
    return 115422;
}
#else
inline NativeProcessId getCurrentNativeThreadId() {
    return ::syscall(SYS_gettid);
}
#endif
}  // namespace

ProcessId ProcessId::getCurrent() {
    return fromNative(getCurrentNativeProcessId());
}

ProcessId ProcessId::getCurrentThreadId() {
    return fromNative(getCurrentNativeThreadId());
}

int64_t ProcessId::asInt64() const {
    typedef std::numeric_limits<NativeProcessId> limits;
    if (limits::is_signed)
        return _npid;
    else
        return static_cast<int64_t>(static_cast<uint64_t>(_npid));
}

long long ProcessId::asLongLong() const {
    return static_cast<long long>(asInt64());
}

std::string ProcessId::toString() const {
    std::ostringstream os;
    os << *this;
    return os.str();
}

std::ostream& operator<<(std::ostream& os, ProcessId pid) {
    return os << pid.toNative();
}

}  // namespace mongo
