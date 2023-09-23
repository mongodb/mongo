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

#include "mongo/util/concurrency/thread_name.h"

#if defined(__APPLE__) || defined(__linux__)
#include <pthread.h>
#endif
#if defined(__APPLE__)
#include <TargetConditionals.h>
#if !TARGET_OS_TV && !TARGET_OS_IOS && !TARGET_OS_WATCH
#include <sys/proc_info.h>
#else
#include <mach/thread_info.h>
#endif
#endif

#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <system_error>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/config.h"     // IWYU pragma: keep
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/process_id.h"
#include "mongo/util/assert_util_core.h"
#include "mongo/util/errno_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {
using namespace fmt::literals;

namespace {

bool isMainThread() {
    return ProcessId::getCurrent() == ProcessId::getCurrentThreadId();
}

#ifdef _WIN32
// From https://msdn.microsoft.com/en-us/library/xcb2z8hs.aspx
// Note: The thread name is only set for the thread if the debugger is attached.

const DWORD MS_VC_EXCEPTION = 0x406D1388;
#pragma pack(push, 8)
typedef struct tagTHREADNAME_INFO {
    DWORD dwType;      // Must be 0x1000.
    LPCSTR szName;     // Pointer to name (in user addr space).
    DWORD dwThreadID;  // Thread ID (-1=caller thread).
    DWORD dwFlags;     // Reserved for future use, must be zero.
} THREADNAME_INFO;
#pragma pack(pop)

void setWindowsThreadName(DWORD dwThreadID, const char* threadName) {
    THREADNAME_INFO info;
    info.dwType = 0x1000;
    info.szName = threadName;
    info.dwThreadID = dwThreadID;
    info.dwFlags = 0;
#pragma warning(push)
#pragma warning(disable : 6320 6322)
    __try {
        RaiseException(MS_VC_EXCEPTION, 0, sizeof(info) / sizeof(ULONG_PTR), (ULONG_PTR*)&info);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
#pragma warning(pop)
}
#endif

void setOSThreadName(const std::string& threadName) {
#if defined(_WIN32)
    // Naming should not be expensive compared to thread creation and connection set up, but if
    // testing shows otherwise we should make this depend on DEBUG again.
    setWindowsThreadName(GetCurrentThreadId(), threadName.c_str());
#elif defined(__APPLE__)
    // Maximum thread name length on OS X is MAXTHREADNAMESIZE (64 characters). This assumes
    // OS X 10.6 or later.
    std::string threadNameCopy = threadName;
    if (threadNameCopy.size() > MAXTHREADNAMESIZE) {
        threadNameCopy.resize(MAXTHREADNAMESIZE - 4);
        threadNameCopy += "...";
    }
    int error = pthread_setname_np(threadNameCopy.c_str());
    if (error) {
        LOGV2(23102,
              "Ignoring error from setting thread name: {error}",
              "Ignoring error from setting thread name",
              "error"_attr = errorMessage(posixError(error)));
    }
#elif defined(__linux__) && defined(MONGO_CONFIG_HAVE_PTHREAD_SETNAME_NP)
    // Do not set thread name on the main() thread. Setting the name on main thread breaks
    // pgrep/pkill since these programs base this name on /proc/*/status which displays the thread
    // name, not the executable name.
    if (isMainThread())
        return;
    //  Maximum thread name length supported on Linux is 16 including the null terminator.
    //  Ideally we use short and descriptive thread names that fit: this helps for log
    //  readability as well. Still, as the limit is so low and a few current names exceed the
    //  limit, it's best to shorten long names.
    static constexpr size_t kMaxThreadNameLength = 16 - 1;
    boost::optional<std::string> shortNameBuf;
    const char* truncName = threadName.c_str();
    if (threadName.size() > kMaxThreadNameLength) {
        StringData sd = threadName;
        shortNameBuf = "{}.{}"_format(sd.substr(0, 7), sd.substr(sd.size() - 7));
        truncName = shortNameBuf->c_str();
    }

    int error = pthread_setname_np(pthread_self(), truncName);
    if (auto ec = posixError(error)) {
        LOGV2(23103,
              "Ignoring error from setting thread name: {error}",
              "Ignoring error from setting thread name",
              "error"_attr = errorMessage(ec));
    }
#endif
}

/**
 * Manages the relationship of our high-level ThreadNameRef strings to
 * the thread local context, and efficiently notifying the OS of name
 * changes. We try to apply temporary names to threads to make them
 * meaningful representations of the kind of work the thread is doing.
 * But sharing these names with the OS is slow and name length is limited.  So
 * ThreadNameInfo is an auxiliary resource to the OS thread name, available to
 * the LOGV2 system and to GDB.
 *
 * ThreadNameInfo are per-thread and managed by thread_local storage.
 *
 * A name is "active" when it has been pushed to the OS by `setHandle`. The
 * association can be abandoned by calling `release`. This doesn't affect the
 * OS, but indicates that the name binding is abandoned and shouldn't be
 * preserved by returning it from subsequent `setHandle` calls. We do however
 * retain the inactive reference in hopes of perhaps identifying redundant
 * `setHandle` calls that would set the OS thread name to the same value it
 * already has.
 *
 * Upon construction, a ThreadNameInfo has an inactive unique name that
 * the OS doesn't know about yet. A push/pop style call sequence of
 * `h=getHandle()` then (eventually) `setHandle(h)` can make this name
 * the active (known to the OS) thread name.
 */
class ThreadNameInfo {
public:
    /** Returns the thread name ref, whether it's active or not. */
    const ThreadNameRef& getHandle() const {
        return _h;
    }

    /**
     * Changes the thread name ref to `name`, marking it active,
     * and updating the OS thread name if necessary.
     *
     * If there was a previous active thread name, it is returned so that
     * callers can perhaps restore it and implement a temporary rename.
     * Inactive thread names are considered abandoned and are not returned.
     */
    ThreadNameRef setHandle(ThreadNameRef name) {
        bool alreadyActive = std::exchange(_active, true);
        if (name == _h)
            return {};
        auto old = std::exchange(_h, std::move(name));
        setOSThreadName(*_h);
        if (alreadyActive)
            return old;
        return {};
    }

    /**
     * Mark the current ThreadNameRef as inactive. This is only a marking and
     * does not affect the OS thread name. The ThreadNameRef is retained
     * so that redundant setHandle calls can be recognized and elided.
     */
    void release() {
        _active = false;
    }

    /**
     * Get a pointer to this thread's ThreadNameInfo.
     * Can return null during thread_local destructors.
     */
    static ThreadNameInfo* forThisThread() {
        struct Tls {
            ~Tls() {
                delete std::exchange(info, nullptr);
            }
            // A pointer has no destructor, so loading it after
            // destruction should be ok.
            ThreadNameInfo* info = new ThreadNameInfo;
        };
        thread_local const Tls tls;
        return tls.info;
    }

private:
    /**
     * Main thread always gets "main". Other threads are sequentially
     * named as "thread1", "thread2", etc.
     */
    static std::string _makeAnonymousThreadName() {
        if (isMainThread())
            return "main";
        static AtomicWord<uint64_t> next{1};
        return "thread{}"_format(next.fetchAndAdd(1));
    }

    ThreadNameRef _h{_makeAnonymousThreadName()};
    bool _active = false;
};

}  // namespace

ThreadNameRef getThreadNameRef() {
    if (auto info = ThreadNameInfo::forThisThread())
        return info->getHandle();
    return {};
}

ThreadNameRef setThreadNameRef(ThreadNameRef name) {
    invariant(name);
    if (auto info = ThreadNameInfo::forThisThread())
        return info->setHandle(std::move(name));
    return {};
}

void releaseThreadNameRef() {
    if (auto info = ThreadNameInfo::forThisThread())
        info->release();
}

}  // namespace mongo
