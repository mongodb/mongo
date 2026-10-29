// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/stdx/thread.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/executor_test_util.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

/** Workaround for bug in MSVC 2022's lambda processor. See SERVER-62480. */
#ifdef _MSC_VER
#define FTU_LAMBDA_R(...) ->__VA_ARGS__
#else
#define FTU_LAMBDA_R(...)
#endif

namespace mongo {

enum DoExecutorFuture : bool {
    // Force exemptions to say *why* they shouldn't test ExecutorFuture to ensure that if the
    // reason stops applying (eg, if we implement ExecutorFuture::tap()) we can delete the enum
    // value and recover the test coverage.
    kNoExecutorFuture_needsTap = false,
    kNoExecutorFuture_needsPromiseSetFrom = false,
    kDoExecutorFuture = true,
};

class [[MONGO_MOD_NEEDS_REPLACEMENT]] DummyInterruptible final : public Interruptible {
    StatusWith<stdx::cv_status> waitForConditionOrInterruptNoAssertUntil(
        stdx::condition_variable& cv, BasicLockableAdapter m, Date_t deadline) noexcept override {
        return Status(ErrorCodes::Interrupted, "");
    }
    Date_t getDeadline() const override {
        MONGO_UNREACHABLE;
    }
    Status checkForInterruptNoAssert() noexcept override {
        // Must be implemented because it's called by Interruptible::waitForConditionOrInterrupt.
        return Status::OK();
    }
    Status checkForDeadlineExpiredNoAssert(Date_t now) noexcept override {
        return Status::OK();
    }
    DeadlineState pushArtificialDeadline(Date_t deadline, ErrorCodes::Error error) override {
        MONGO_UNREACHABLE;
    }
    void popArtificialDeadline(DeadlineState) override {
        MONGO_UNREACHABLE;
    }
    Date_t getExpirationDateForWaitForValue(Milliseconds waitFor) override {
        return Date_t::now() + waitFor;
    }
};

template <typename T, typename Func>
void completePromise(Promise<T>* promise, Func&& func) {
    promise->emplaceValue(func());
}

template <typename Func>
void completePromise(Promise<void>* promise, Func&& func) {
    func();
    promise->emplaceValue();
}

inline void sleepIfShould() {
#if !__has_feature(thread_sanitizer)
    // TSAN and rr work better without this sleep, but it is useful for testing correctness.
    static const bool runningUnderRR = getenv("RUNNING_UNDER_RR") != nullptr;
    if (!runningUnderRR)
        sleepmillis(100);  // Try to wait until after the Future has been handled.
#endif
}

template <typename Func, typename Result = std::invoke_result_t<Func&&>>
Future<Result> async(Func&& func) {
    auto pf = makePromiseFuture<Result>();

    stdx::thread([promise = std::move(pf.promise), func = std::forward<Func>(func)]() mutable {
        sleepIfShould();
        try {
            completePromise(&promise, func);
        } catch (const DBException& ex) {
            promise.setError(ex.toStatus());
        }
    }).detach();

    return std::move(pf.future);
}

inline Status failStatus() {
    return Status(ErrorCodes::Error(50728), "expected failure");
}

#define ASSERT_THROWS_failStatus(expr)                                          \
    [&] {                                                                       \
        ASSERT_THROWS_WITH_CHECK(expr, DBException, [](const DBException& ex) { \
            ASSERT_EQ(ex.toStatus(), failStatus());                             \
        });                                                                     \
    }()

// Tests a Future completed by completionExpr using testFunc. The Future will be completed in
// various ways to maximize test coverage.
template <DoExecutorFuture doExecutorFuture = kDoExecutorFuture,
          typename CompletionFunc,
          typename TestFunc,
          typename = std::enable_if_t<!std::is_void<std::invoke_result_t<CompletionFunc>>::value>>
void FUTURE_SUCCESS_TEST(const CompletionFunc& completion, const TestFunc& test) {
    using CompletionType = decltype(completion());
    {  // immediate future
        test(Future<CompletionType>::makeReady(completion()));
    }
    {  // ready future from promise
        auto pf = makePromiseFuture<CompletionType>();
        pf.promise.emplaceValue(completion());
        test(std::move(pf.future));
    }

    {  // async future
        test(async([&] { return completion(); }));
    }

    if constexpr (doExecutorFuture) {  // immediate executor future
        auto exec = InlineQueuedCountingExecutor::make();
        test(Future<CompletionType>::makeReady(completion()).thenRunOn(exec));
    }
}

template <DoExecutorFuture doExecutorFuture = kDoExecutorFuture,
          typename CompletionFunc,
          typename TestFunc,
          typename = std::enable_if_t<std::is_void<std::invoke_result_t<CompletionFunc>>::value>,
          typename = void>
void FUTURE_SUCCESS_TEST(const CompletionFunc& completion, const TestFunc& test) {
    using CompletionType = decltype(completion());
    {  // immediate future
        completion();
        test(Future<CompletionType>::makeReady());
    }
    {  // ready future from promise
        auto pf = makePromiseFuture<CompletionType>();
        completion();
        pf.promise.emplaceValue();
        test(std::move(pf.future));
    }

    {  // async future
        test(async([&] { return completion(); }));
    }

    if constexpr (doExecutorFuture) {  // immediate executor future
        completion();
        auto exec = InlineQueuedCountingExecutor::make();
        test(Future<CompletionType>::makeReady().thenRunOn(exec));
    }
}

template <typename CompletionType,
          DoExecutorFuture doExecutorFuture = kDoExecutorFuture,
          typename TestFunc>
void FUTURE_FAIL_TEST(const TestFunc& test) {
    {  // immediate future
        test(Future<CompletionType>::makeReady(failStatus()));
    }
    {  // ready future from promise
        auto pf = makePromiseFuture<CompletionType>();
        pf.promise.setError(failStatus());
        test(std::move(pf.future));
    }

    {  // async future
        test(async([&]() -> CompletionType {
            uassertStatusOK(failStatus());
            MONGO_UNREACHABLE;
        }));
    }
    if constexpr (doExecutorFuture) {  // immediate executor future
        auto exec = InlineQueuedCountingExecutor::make();
        test(Future<CompletionType>::makeReady(failStatus()).thenRunOn(exec));
    }
}

/**
 * True if PromiseT::setFrom(ArgT) is valid.
 */
template <typename PromiseT, typename ArgT, typename = void>
inline constexpr bool canSetFrom = false;

template <typename PromiseT>
inline constexpr bool canSetFrom<PromiseT,  //
                                 void,      //
                                 decltype(std::declval<PromiseT&>().setFrom())> = true;

template <typename PromiseT, typename ArgT>
inline constexpr bool
    canSetFrom<PromiseT,  //
               ArgT,      //
               decltype(std::declval<PromiseT&>().setFrom(std::declval<ArgT>()))> = true;

}  // namespace mongo
