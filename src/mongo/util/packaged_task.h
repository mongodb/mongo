// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/functional.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

#include <type_traits>
#include <utility>

namespace mongo {
namespace packaged_task_detail {
/**
 * SigHelper is a family of types helpful for deducing the type signature of the callable wrapped by
 * a PackagedTask.
 */
template <typename>
struct SigHelper {};
// Function Type
template <typename Ret, typename... Args>
struct SigHelper<Ret (*)(Args...)> : std::type_identity<Ret(Args...)> {};
// Member Function Pointers
template <typename Class, typename Ret, typename... Args>
struct SigHelper<Ret (Class::*)(Args...)> : std::type_identity<Ret(Args...)> {};
template <typename Class, typename Ret, typename... Args>
struct SigHelper<Ret (Class::*)(Args...)&> : std::type_identity<Ret(Args...)> {};
template <typename Class, typename Ret, typename... Args>
struct SigHelper<Ret (Class::*)(Args...) const> : std::type_identity<Ret(Args...)> {};
template <typename Class, typename Ret, typename... Args>
struct SigHelper<Ret (Class::*)(Args...) const&> : std::type_identity<Ret(Args...)> {};

template <typename T>
using getCallOperator = decltype(&T::operator());

template <typename T>
constexpr bool hasCallOperator = stdx::is_detected_v<getCallOperator, T>;

template <typename Callable>
using SigFor = typename std::conditional_t<hasCallOperator<Callable>,
                                           SigHelper<decltype(&Callable::operator())>,
                                           SigHelper<Callable>>::type;
}  // namespace packaged_task_detail

/**
 * A PackagedTask wraps anything Callable, but packages the return value of the Callable in a Future
 * that can be accessed before the Callable is run. Construct a PackagedTask by giving it a
 * Callable. Once the PackagedTask is constructed, you can extract a Future that will contain the
 * result of running the packaged task. The PackagedTask can be invoked as if it was the Callable
 * that it wraps.
 */
template <typename Sig>
class PackagedTask;
template <typename R, typename... Args>
class PackagedTask<R(Args...)> {
    using ReturnType = FutureContinuationResult<unique_function<R(Args...)>, Args...>;

public:
    template <typename F>
    explicit PackagedTask(F&& f) : _f(std::forward<F>(f)) {}
    PackagedTask(const PackagedTask&) = delete;
    PackagedTask& operator=(const PackagedTask&) = delete;
    PackagedTask(PackagedTask&&) = default;
    PackagedTask& operator=(PackagedTask&&) = default;

    /**
     * Invokes the Callable wrapped by this PackagedTask. This can only be called once, as a
     * PackagedTask produces at most one result obtained from running the wrapped Callable at most
     * one time. It is invalid to call this more than once.
     */
    void operator()(Args... args) {
        _p.setWith([&] { return _f(std::forward<Args>(args)...); });
    }

    /**
     * Returns a Future that represents the (possibly-deferred) result of the wrapped task. Because
     * running the task will produce exactly one result, it is safe to call getFuture() at most once
     * on any PackagedTask; subsequent calls will throw a DBException set with
     * ErrorCodes::FutureAlreadyRetrieved.
     */
    Future<ReturnType> getFuture() {
        if (_futureExtracted) {
            iasserted(ErrorCodes::FutureAlreadyRetrieved,
                      "Attempted to extract more than one future from a PackagedTask");
        }
        _futureExtracted = true;
        return std::move(_fut);
    }

private:
    unique_function<R(Args...)> _f;
    Promise<ReturnType> _p{NonNullPromiseTag{}};
    Future<ReturnType> _fut{_p.getFuture()};
    bool _futureExtracted{false};
};

template <typename F, typename Sig = packaged_task_detail::SigFor<F>>
PackagedTask(F&& f) -> PackagedTask<Sig>;

template <typename R, typename... Args>
PackagedTask(R (*)(Args...)) -> PackagedTask<R(Args...)>;
}  // namespace mongo
