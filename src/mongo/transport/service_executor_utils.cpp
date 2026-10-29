// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/service_executor_utils.h"

#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>

#include <fmt/format.h>

#if !defined(_WIN32)
#include <pthread.h>

#include <sys/resource.h>
#endif

#include "mongo/base/error_codes.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/synchronized_value.h"
#include "mongo/util/thread_safety_context.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo::transport {

namespace {

// Client thread renice counters. Production reporting is owned by whichever module registers a
// ClientThreadNiceMetricsSink, see below.
Atomic<int64_t> gClientThreadsRenicedCount{0};
Atomic<int64_t> gClientThreadReniceFailedCount{0};
Atomic<bool> gLoggedFirstRenice{false};
Atomic<bool> gLoggedReniceFailure{false};

synchronized_value<ClientThreadNiceEligibilityPredicate> gClientThreadNiceEligibilityPredicate;

bool isClientThreadNiceEligible() {
    auto predicate = gClientThreadNiceEligibilityPredicate.synchronize();
    return *predicate && (*predicate)();
}

synchronized_value<ClientThreadNiceValueProvider> gClientThreadNiceValueProvider;

int32_t getClientThreadNiceValue() {
    auto provider = gClientThreadNiceValueProvider.synchronize();
    return *provider ? (*provider)() : 0;
}

synchronized_value<ClientThreadNiceMetricsSink> gClientThreadNiceMetricsSink;

void reportNiceValueObserved(int32_t niceValue) {
    auto sink = gClientThreadNiceMetricsSink.synchronize();
    if (sink->onNiceValueObserved) {
        sink->onNiceValueObserved(niceValue);
    }
}

void reportThreadReniced() {
    auto sink = gClientThreadNiceMetricsSink.synchronize();
    if (sink->onThreadReniced) {
        sink->onThreadReniced();
    }
}

void reportThreadReniceFailed() {
    auto sink = gClientThreadNiceMetricsSink.synchronize();
    if (sink->onThreadReniceFailed) {
        sink->onThreadReniceFailed();
    }
}

void* runFunc(void* ctx) {
    auto taskPtr =
        std::unique_ptr<unique_function<void()>>(static_cast<unique_function<void()>*>(ctx));
    (*taskPtr)();

    return nullptr;
}
}  // namespace

int64_t getClientThreadsRenicedCount() {
    return gClientThreadsRenicedCount.load();
}

int64_t getClientThreadReniceFailedCount() {
    return gClientThreadReniceFailedCount.load();
}

void registerClientThreadNiceEligibilityPredicate(ClientThreadNiceEligibilityPredicate predicate) {
    *gClientThreadNiceEligibilityPredicate.synchronize() = std::move(predicate);
}

void registerClientThreadNiceMetricsSink(ClientThreadNiceMetricsSink sink) {
    *gClientThreadNiceMetricsSink.synchronize() = std::move(sink);
}

void registerClientThreadNiceValueProvider(ClientThreadNiceValueProvider provider) {
    *gClientThreadNiceValueProvider.synchronize() = std::move(provider);
}

void applyClientThreadNiceValue() {
    const int32_t niceValue = getClientThreadNiceValue();
    reportNiceValueObserved(niceValue);

    if (niceValue <= 0) {
        return;
    }

    if (!isClientThreadNiceEligible()) {
        return;
    }

#ifdef __linux__
    // On Linux each thread is a task with its own nice value, and `PRIO_PROCESS` with `who == 0`
    // targets the calling thread's TID, which is why this runs on the newly spawned client thread
    // rather than on the acceptor.
    errno = 0;
    if (setpriority(PRIO_PROCESS, 0, niceValue) != 0) {
        auto ec = lastSystemError();
        gClientThreadReniceFailedCount.fetchAndAddRelaxed(1);
        reportThreadReniceFailed();
        if (!gLoggedReniceFailure.swap(true)) {
            LOGV2_WARNING(13483700,
                          "Failed to apply client thread nice value to a client connection thread; "
                          "the connection will proceed at the default scheduling priority. This "
                          "is logged once per process",
                          "niceValue"_attr = niceValue,
                          "errno"_attr = ec.value(),
                          "error"_attr = errorMessage(ec));
        }
        return;
    }

    gClientThreadsRenicedCount.fetchAndAddRelaxed(1);
    reportThreadReniced();
    if (!gLoggedFirstRenice.swap(true)) {
        LOGV2(13483701,
              "Applying client thread nice value to client connection threads; client threads will "
              "run at a lower CFS priority than internal threads. This is logged once per process",
              "niceValue"_attr = niceValue);
    }
    LOGV2_DEBUG(13483702,
                2,
                "Applied client thread nice value to client connection thread",
                "niceValue"_attr = niceValue);
#else
    // Not supported outside Linux; log once so a misconfigured arm is visible.
    if (!gLoggedReniceFailure.swap(true)) {
        LOGV2_WARNING(13483703,
                      "Client thread nice value is set but is only supported on Linux; ignoring",
                      "niceValue"_attr = niceValue);
    }
#endif
}

Status launchServiceWorkerThread(unique_function<void()> task) {

    try {
#if defined(_WIN32)
        stdx::thread([task = std::move(task)]() mutable {
            applyClientThreadNiceValue();
            task();
        }).detach();
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
                              "pthread_attr_setstacksize failed",
                              "error"_attr = errorMessage(posixError(failed)));
            }
        } else {
            LOGV2_WARNING(22950,
                          "Stack size not set to suggested 1024KiB",
                          "stackSizeKiB"_attr = (limits.rlim_cur / 1024));
        }

        // Wrap the user-specified `task` so it runs with an installed `sigaltstack`.
        task = [sigAltStackController = std::make_shared<stdx::support::SigAltStackController>(),
                f = std::move(task)]() mutable {
            auto sigAltStackGuard = sigAltStackController->makeInstallGuard();
            applyClientThreadNiceValue();
            f();
        };

        pthread_t thread;
        auto ctx = std::make_unique<unique_function<void()>>(std::move(task));
        ThreadSafetyContext::getThreadSafetyContext()->onThreadCreate();

        int failed = pthread_create(&thread, &attrs, runFunc, ctx.get());
        if (failed > 0) {
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

        // The spawned thread takes over ownership, cast to void to explicitly ignore the return
        // value.
        (void)ctx.release();
#endif

    } catch (const std::exception& e) {
        LOGV2_ERROR(22948, "Thread creation failed", "error"_attr = e.what());
        return {ErrorCodes::InternalError,
                fmt::format("Failed to create service entry worker thread: {}", e.what())};
    }

    return Status::OK();
}

}  // namespace mongo::transport
