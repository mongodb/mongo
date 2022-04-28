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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

#include "mongo/platform/basic.h"

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
#if defined(__linux__)
#include <sys/syscall.h>
#include <sys/types.h>
#endif

#include <fmt/format.h>

#include "mongo/base/init.h"
#include "mongo/config.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/str.h"

namespace mongo {
using namespace fmt::literals;

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

constexpr auto kMainId = size_t{0};

auto makeAnonymousThreadName() {
    static auto gNextAnonymousId = AtomicWord<size_t>{kMainId};
    auto id = gNextAnonymousId.fetchAndAdd(1);
    if (id == kMainId) {
        // The first thread name should always be "main".
        return make_intrusive<ThreadName>("main");
    } else {
        return make_intrusive<ThreadName>("thread{}"_format(id));
    }
}

struct ThreadNameSconce {
    ThreadNameSconce() : cachedPtr(makeAnonymousThreadName()) {
        // Note that we're not setting the thread name here. It will log differently, but appear the
        // same in top and like.
    }

    // At any given time, either cachedPtr or activePtr can be valid, but not both.
    boost::intrusive_ptr<ThreadName> activePtr;
    boost::intrusive_ptr<ThreadName> cachedPtr;
};

auto getSconce = ThreadContext::declareDecoration<ThreadNameSconce>();
auto& getThreadName(const boost::intrusive_ptr<ThreadContext>& context) {
    auto& sconce = getSconce(context.get());
    if (sconce.activePtr) {
        return sconce.activePtr;
    }

    return sconce.cachedPtr;
}

void setOSThreadName(StringData threadName) {
#if defined(_WIN32)
    // Naming should not be expensive compared to thread creation and connection set up, but if
    // testing shows otherwise we should make this depend on DEBUG again.
    setWindowsThreadName(GetCurrentThreadId(), threadName.rawData());
#elif defined(__APPLE__)
    // Maximum thread name length on OS X is MAXTHREADNAMESIZE (64 characters). This assumes
    // OS X 10.6 or later.
    std::string threadNameCopy = threadName.toString();
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
    if (getpid() != syscall(SYS_gettid)) {
        //  Maximum thread name length supported on Linux is 16 including the null terminator.
        //  Ideally we use short and descriptive thread names that fit: this helps for log
        //  readability as well. Still, as the limit is so low and a few current names exceed the
        //  limit, it's best to shorten long names.
        int error = 0;
        if (threadName.size() > 15) {
            std::string shortName = str::stream()
                << threadName.substr(0, 7) << '.' << threadName.substr(threadName.size() - 7);
            error = pthread_setname_np(pthread_self(), shortName.c_str());
        } else {
            error = pthread_setname_np(pthread_self(), threadName.rawData());
        }

        if (error) {
            LOGV2(23103,
                  "Ignoring error from setting thread name: {error}",
                  "Ignoring error from setting thread name",
                  "error"_attr = errorMessage(posixError(error)));
        }
    }
#endif
}

}  // namespace

ThreadName::Id ThreadName::_nextId() {
    static auto gNextId = AtomicWord<Id>{0};
    return gNextId.fetchAndAdd(1);
}

StringData ThreadName::getStaticString() {
    auto& context = ThreadContext::get();
    if (!context) {
        // Use a static fallback to avoid allocations. This is the string that will be used before
        // initializers run in main a.k.a. pre-init.
        static constexpr auto kFallback = "-"_sd;
        return kFallback;
    }

    return getThreadName(context)->toString();
}

boost::intrusive_ptr<ThreadName> ThreadName::get(boost::intrusive_ptr<ThreadContext> context) {
    return getThreadName(context);
}

boost::intrusive_ptr<ThreadName> ThreadName::set(boost::intrusive_ptr<ThreadContext> context,
                                                 boost::intrusive_ptr<ThreadName> name) {
    invariant(name);

    auto& sconce = getSconce(context.get());

    if (sconce.activePtr) {
        invariant(!sconce.cachedPtr);
        if (*sconce.activePtr == *name) {
            // The name was already set, skip setting it to the OS thread name.
            return {};
        } else {
            // Replace the current active name with the new one, and set the OS thread name.
            setOSThreadName(name->toString());
            return std::exchange(sconce.activePtr, name);
        }
    } else if (sconce.cachedPtr) {
        if (*sconce.cachedPtr == *name) {
            // The name was cached, set it as active and skip setting it to the OS thread name.
            sconce.activePtr = std::exchange(sconce.cachedPtr, {});
            return {};
        } else {
            // The new name is different than the cached name, set the active, reset the cached, and
            // set the OS thread name.
            setOSThreadName(name->toString());

            sconce.activePtr = name;
            sconce.cachedPtr.reset();
            return {};
        }
    }

    MONGO_UNREACHABLE;
}

void ThreadName::release(boost::intrusive_ptr<ThreadContext> context) {
    auto& sconce = getSconce(context.get());
    if (sconce.activePtr) {
        sconce.cachedPtr = std::exchange(sconce.activePtr, {});
    }
}

ThreadName::ThreadName(StringData name) : _id(_nextId()), _storage(name.toString()){};

}  // namespace mongo
