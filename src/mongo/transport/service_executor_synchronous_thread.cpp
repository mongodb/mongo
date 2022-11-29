/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include <fmt/format.h>

#include "mongo/base/status.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/functional.h"
#include "mongo/util/thread_safety_context.h"

#if !defined(_WIN32)
#include <sys/resource.h>
#include <unistd.h>
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kExecutor

namespace mongo::transport {

namespace {

using namespace fmt::literals;

MONGO_FAIL_POINT_DEFINE(serviceExecutorSynchronousThreadFailToSpawn);

#if !defined(_WIN32)

template <typename T>
auto roundToNext(T n, T mod) {
    return (n + mod - 1) / mod * mod;
}

size_t getStackSizeResourceLimit() {
    struct rlimit rlim;
    if (getrlimit(RLIMIT_STACK, &rlim)) {
        auto ec = lastPosixError();
        iasserted(ErrorCodes::InternalError, "getrlimit:{}"_format(errorMessage(ec)));
    }
    return rlim.rlim_cur;
}

size_t getSystemPageSize() {
    auto r = sysconf(_SC_PAGE_SIZE);
    if (r == -1) {
        auto ec = lastPosixError();
        iasserted(ErrorCodes::InternalError, "sysconf:{}"_format(errorMessage(ec)));
    }
    return r;
}

void configureStackSize(pthread_attr_t* attrs) {
    static constexpr size_t kSuggested = 1 << 20;  // 1MiB
    auto rlim = getStackSizeResourceLimit();
    iassert(ErrorCodes::InternalError,
            "Small stack size resource limit. rlim={}, suggested={}"_format(rlim, kSuggested),
            rlim >= kSuggested);
    size_t sz = kSuggested;

#if __SANITIZE_ADDRESS__ || __has_feature(address_sanitizer)
    // If we are using address sanitizer, we set the stack at
    // ~75% (rounded up to a multiple of the page size) of our
    // usual desired. Since ASAN is known to use stack more
    // aggressively and should positively detect stack overflow,
    // this gives us increased confidence during testing that we
    // aren't flirting with our real 1MB limit for any tested
    // workloads. Note: This calculation only works on POSIX
    // platforms. If we ever decide to use the MSVC
    // implementation of ASAN, we will need to revisit it.
    sz = roundToNext(sz * 3 / 4, getSystemPageSize());
#endif

    if (int failed = pthread_attr_setstacksize(attrs, sz)) {
        auto ec = posixError(failed);
        iasserted(ErrorCodes::InternalError,
                  "pthread_attr_setstacksize: {}"_format(errorMessage(ec)));
    }
}

#endif  // !_WIN32

}  // namespace

Status launchServiceWorkerThread(unique_function<void()> task) try {
    if (serviceExecutorSynchronousThreadFailToSpawn.shouldFail())
        iasserted(7015118, "Injected spawn failure");
#if defined(_WIN32)
    stdx::thread([task = std::move(task)]() mutable { task(); }).detach();
#else
    pthread_attr_t attrs;
    pthread_attr_init(&attrs);
    ScopeGuard attrsGuard([&attrs] { pthread_attr_destroy(&attrs); });
    pthread_attr_setdetachstate(&attrs, PTHREAD_CREATE_DETACHED);

    try {
        configureStackSize(&attrs);
    } catch (const DBException& ex) {
        LOGV2_WARNING(5017170, "Failed to configure stack size. Using default", "error"_attr = ex);
    }

    // Wrap the user-specified `task` so it runs with an installed `sigaltstack`.
    task = [sigAltStackController = std::make_shared<stdx::support::SigAltStackController>(),
            f = std::move(task)]() mutable {
        auto sigAltStackGuard = sigAltStackController->makeInstallGuard();
        f();
    };

    struct ThreadData {
        ThreadData(unique_function<void()> f) : f{std::move(f)} {}
        unique_function<void()> f;
    };
    auto td = std::make_unique<ThreadData>(std::move(task));
    auto threadBody = +[](void* arg) -> void* {
        std::unique_ptr<ThreadData>(static_cast<ThreadData*>(arg))->f();
        return nullptr;
    };

    pthread_t thread;
    ThreadSafetyContext::getThreadSafetyContext()->onThreadCreate();
    if (int failed = pthread_create(&thread, &attrs, threadBody, td.get()); failed > 0) {
        LOGV2_ERROR_OPTIONS(4850900,
                            {logv2::UserAssertAfterLog()},
                            "pthread_create failed",
                            "error"_attr = errorMessage(posixError(failed)));
    } else if (failed < 0) {
        auto ec = lastPosixError();
        LOGV2_ERROR_OPTIONS(4850901,
                            {logv2::UserAssertAfterLog()},
                            "pthread_create failed with a negative return code",
                            "code"_attr = failed,
                            "errno"_attr = ec.value(),
                            "error"_attr = errorMessage(ec));
    }
    td.release();
#endif

    return Status::OK();
} catch (const std::exception& e) {
    LOGV2_ERROR(22948, "Thread creation failed", "error"_attr = e.what());
    return {ErrorCodes::InternalError,
            "Failed to create service entry worker thread: {}"_format(e.what())};
}

}  // namespace mongo::transport
