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
