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

#include "mongo/transport/service_executor_utils.h"

#include <cstddef>
#include <exception>
#include <fmt/format.h>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

#if !defined(_WIN32)
#include <pthread.h>
#include <sys/resource.h>
#endif

#include "mongo/base/error_codes.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_options.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/thread_safety_context.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo::transport {

namespace {
using namespace fmt::literals;

void* runFunc(void* ctx) {
    auto taskPtr =
        std::unique_ptr<unique_function<void()>>(static_cast<unique_function<void()>*>(ctx));
    (*taskPtr)();

    return nullptr;
}
}  // namespace

Status launchServiceWorkerThread(unique_function<void()> task) {

    try {
#if defined(_WIN32)
        stdx::thread([task = std::move(task)]() mutable { task(); }).detach();
#else
        pthread_attr_t attrs;
        pthread_attr_init(&attrs);
        ScopeGuard attrsGuard([&attrs] { pthread_attr_destroy(&attrs); });
        pthread_attr_setdetachstate(&attrs, PTHREAD_CREATE_DETACHED);

        static const rlim_t kStackSize =
            1024 * 1024;  // if we change this we need to update the warning

        struct rlimit limits;
        invariant(getrlimit(RLIMIT_STACK, &limits) == 0);
        if (limits.rlim_cur >= kStackSize) {

            size_t stackSizeToSet = kStackSize;

#if !defined(_WIN32) && (__SANITIZE_ADDRESS__ || __has_feature(address_sanitizer))
            // If we are using address sanitizer, we set the stack at
            // ~75% (rounded up to a multiple of the page size) of our
            // usual desired. Since ASAN is known to use stack more
            // aggressively and should positively detect stack overflow,
            // this gives us increased confidence during testing that we
            // aren't flirting with our real 1MB limit for any tested
            // workloads. Note: This calculation only works on POSIX
            // platforms. If we ever decide to use the MSVC
            // implementation of ASAN, we will need to revisit it.
            long page_size = sysconf(_SC_PAGE_SIZE);
            stackSizeToSet =
                ((((stackSizeToSet * 3) >> 2) + page_size - 1) / page_size) * page_size;
#endif
            int failed = pthread_attr_setstacksize(&attrs, stackSizeToSet);
            if (failed) {
                LOGV2_WARNING(22949,
                              "pthread_attr_setstacksize failed: {error}",
                              "pthread_attr_setstacksize failed",
                              "error"_attr = errorMessage(posixError(failed)));
            }
        } else {
            LOGV2_WARNING(22950,
                          "Stack size set to {stackSizeKiB}KiB. We suggest 1024KiB",
                          "Stack size not set to suggested 1024KiB",
                          "stackSizeKiB"_attr = (limits.rlim_cur / 1024));
        }

        // Wrap the user-specified `task` so it runs with an installed `sigaltstack`.
        task = [sigAltStackController = std::make_shared<stdx::support::SigAltStackController>(),
                f = std::move(task)]() mutable {
            auto sigAltStackGuard = sigAltStackController->makeInstallGuard();
            f();
        };

        pthread_t thread;
        auto ctx = std::make_unique<unique_function<void()>>(std::move(task));
        ThreadSafetyContext::getThreadSafetyContext()->onThreadCreate();

        int failed = pthread_create(&thread, &attrs, runFunc, ctx.get());
        if (failed > 0) {
            LOGV2_ERROR_OPTIONS(4850900,
                                {logv2::UserAssertAfterLog()},
                                "pthread_create failed: error: {error}",
                                "pthread_create failed",
                                "error"_attr = errorMessage(posixError(failed)));
        } else if (failed < 0) {
            auto ec = lastPosixError();
            LOGV2_ERROR_OPTIONS(4850901,
                                {logv2::UserAssertAfterLog()},
                                "pthread_create failed with a negative return code: {code}, errno: "
                                "{errno}, error: {error}",
                                "pthread_create failed with a negative return code",
                                "code"_attr = failed,
                                "errno"_attr = ec.value(),
                                "error"_attr = errorMessage(ec));
        }

        // The spawned thread takes over ownership, cast to void to explicitly ignore the return
        // value.
        (void)ctx.release();
#endif

    } catch (const std::exception& e) {
        LOGV2_ERROR(22948, "Thread creation failed", "error"_attr = e.what());
        return {ErrorCodes::InternalError,
                format(FMT_STRING("Failed to create service entry worker thread: {}"), e.what())};
    }

    return Status::OK();
}

}  // namespace mongo::transport
