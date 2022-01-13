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
#pragma once

#include "mongo/base/error_codes.h"

#include "mongo/stdx/type_traits.h"
#include "mongo/util/future.h"

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
struct SigHelper<Ret (*)(Args...)> : stdx::type_identity<Ret(Args...)> {};
// Member Function Pointers
template <typename Class, typename Ret, typename... Args>
struct SigHelper<Ret (Class::*)(Args...)> : stdx::type_identity<Ret(Args...)> {};
template <typename Class, typename Ret, typename... Args>
struct SigHelper<Ret (Class::*)(Args...)&> : stdx::type_identity<Ret(Args...)> {};
template <typename Class, typename Ret, typename... Args>
struct SigHelper<Ret (Class::*)(Args...) const> : stdx::type_identity<Ret(Args...)> {};
template <typename Class, typename Ret, typename... Args>
struct SigHelper<Ret (Class::*)(Args...) const&> : stdx::type_identity<Ret(Args...)> {};

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
PackagedTask(F&& f)->PackagedTask<Sig>;

template <typename R, typename... Args>
PackagedTask(R (*)(Args...))->PackagedTask<R(Args...)>;
}  // namespace mongo
