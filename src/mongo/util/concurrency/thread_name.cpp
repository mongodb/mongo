/*    Copyright 2009 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/platform/basic.h"

#include "mongo/util/concurrency/thread_name.h"

#if defined(__APPLE__) || defined(__linux__)
#include <pthread.h>
#endif
#if defined(__APPLE__)
#include <TargetConditionals.h>
#if !TARGET_OS_TV && !TARGET_OS_IOS
#include <sys/proc_info.h>
#else
#include <mach/thread_info.h>
#endif
#endif
#if defined(__linux__)
#include <sys/syscall.h>
#include <sys/types.h>
#endif

#include "mongo/base/init.h"
#include "mongo/config.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::string;

namespace {

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

AtomicInt64 nextUnnamedThreadId{1};

// It is unsafe to access threadName before its dynamic initialization has completed. Use
// the execution of mongo initializers (which only happens once we have entered main, and
// therefore after dynamic initialization is complete) to signal that it is safe to use
// 'threadName'.
bool mongoInitializersHaveRun{};
MONGO_INITIALIZER(ThreadNameInitializer)(InitializerContext*) {
    mongoInitializersHaveRun = true;
    // The global initializers should only ever be run from main, so setting thread name
    // here makes sense.
    setThreadName("main");
    return Status::OK();
}

// TODO consider making threadName std::string and removing the size limit once we get real
// thread_local.
constexpr size_t kMaxThreadNameSize = 63;
thread_local char threadNameStorage[kMaxThreadNameSize + 1];

}  // namespace

namespace for_debuggers {
// This needs external linkage to ensure that debuggers can use it.
thread_local StringData threadName;
}
using for_debuggers::threadName;

void setThreadName(StringData name) {
    invariant(mongoInitializersHaveRun);
    if (name.size() > kMaxThreadNameSize) {
        // Truncate unreasonably long thread names.
        name = name.substr(0, kMaxThreadNameSize);
    }
    name.copyTo(threadNameStorage, /*null terminate=*/true);
    threadName = StringData(threadNameStorage, name.size());

#if defined(_WIN32)
    // Naming should not be expensive compared to thread creation and connection set up, but if
    // testing shows otherwise we should make this depend on DEBUG again.
    setWindowsThreadName(GetCurrentThreadId(), threadName.rawData());
#elif defined(__APPLE__)
    // Maximum thread name length on OS X is MAXTHREADNAMESIZE (64 characters). This assumes
    // OS X 10.6 or later.
    MONGO_STATIC_ASSERT(MAXTHREADNAMESIZE >= kMaxThreadNameSize + 1);
    int error = pthread_setname_np(threadName.rawData());
    if (error) {
        log() << "Ignoring error from setting thread name: " << errnoWithDescription(error);
    }
#elif defined(__linux__) && defined(MONGO_CONFIG_HAVE_PTHREAD_SETNAME_NP)
    // Do not set thread name on the main() thread. Setting the name on main thread breaks
    // pgrep/pkill since these programs base this name on /proc/*/status which displays the thread
    // name, not the executable name.
    if (getpid() != syscall(SYS_gettid)) {
        //  Maximum thread name length supported on Linux is 16 including the null terminator.
        //  Ideally we use short and descriptive thread names that fit: this helps for log
        //  readability as well. Still, as the limit is so low and a few current names exceed the
        //  limit, it's best to shorten long names.
        int error = 0;
        if (threadName.size() > 15) {
            std::string shortName = str::stream() << threadName.substr(0, 7) << '.'
                                                  << threadName.substr(threadName.size() - 7);
            error = pthread_setname_np(pthread_self(), shortName.c_str());
        } else {
            error = pthread_setname_np(pthread_self(), threadName.rawData());
        }

        if (error) {
            log() << "Ignoring error from setting thread name: " << errnoWithDescription(error);
        }
    }
#endif
}

StringData getThreadName() {
    if (MONGO_unlikely(!mongoInitializersHaveRun)) {
        // 'getThreadName' has been called before dynamic initialization for this
        // translation unit has completed, so return a fallback value rather than accessing
        // the 'threadName' variable, which requires dynamic initialization. We assume that
        // we are in the 'main' thread.
        static const std::string kFallback = "main";
        return kFallback;
    }

    if (threadName.empty()) {
        setThreadName(str::stream() << "thread" << nextUnnamedThreadId.fetchAndAdd(1));
    }
    return threadName;
}

}  // namespace mongo
