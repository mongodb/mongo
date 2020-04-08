/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/util/future.h"

namespace mongo {

/**
 * Returns a future which will be fulfilled at the given date.
 */
ExecutorFuture<void> sleepUntil(std::shared_ptr<executor::TaskExecutor> executor,
                                const Date_t& date) {
    auto [promise, future] = makePromiseFuture<void>();
    auto taskCompletionPromise = std::make_shared<Promise<void>>(std::move(promise));

    auto scheduledWorkHandle = executor->scheduleWorkAt(
        date, [taskCompletionPromise](const executor::TaskExecutor::CallbackArgs& args) mutable {
            if (args.status.isOK()) {
                taskCompletionPromise->emplaceValue();
            } else {
                taskCompletionPromise->setError(args.status);
            }
        });

    if (!scheduledWorkHandle.isOK()) {
        taskCompletionPromise->setError(scheduledWorkHandle.getStatus());
    }
    return std::move(future).thenRunOn(executor);
}

/**
 * Returns a future which will be fulfilled after the given duration.
 */
ExecutorFuture<void> sleepFor(std::shared_ptr<executor::TaskExecutor> executor,
                              Milliseconds duration) {
    return sleepUntil(executor, executor->now() + duration);
}

namespace future_util_details {

/**
 * Widget to get a default-constructible object that allows access to the type passed in at
 * compile time. Used for getReturnType below.
 */
template <typename T>
struct DefaultConstructibleWrapper {
    using type = T;
};

/**
 * Helper to get the return type of the loop body in TryUntilLoop/TryUntilLoopWithDelay. This is
 * required because the loop body may return a future-like type, which wraps another result type
 * (specified in Future<T>::value_type), or some other kind of raw type which can be used directly.
 */
template <typename T>
auto getReturnType() {
    if constexpr (future_details::isFutureLike<std::decay_t<T>>) {
        return DefaultConstructibleWrapper<typename T::value_type>();
    } else {
        return DefaultConstructibleWrapper<T>();
    }
}

/**
 * Represents an intermediate state which holds the body, condition, and delay between iterations of
 * a try-until loop.  See comments for AsyncTry for usage.
 *
 * Note: This is a helper class and is not intended for standalone usage.
 */
template <typename BodyCallable, typename ConditionCallable, typename Delay>
class [[nodiscard]] AsyncTryUntilWithDelay {
public:
    explicit AsyncTryUntilWithDelay(
        BodyCallable && body, ConditionCallable && condition, Delay delay)
        : _body(std::move(body)), _condition(std::move(condition)), _delay(delay) {}

    /**
     * Launches the loop and returns an ExecutorFuture that will be resolved when the loop is
     * complete.
     *
     * The returned ExecutorFuture contains the last result returned by the loop body. If the last
     * iteration of the loop body threw an exception or otherwise returned an error status, the
     * returned ExecutorFuture will contain that error.
     */
    auto on(std::shared_ptr<executor::TaskExecutor> executor)&& {
        auto loop = std::make_shared<TryUntilLoopWithDelay>(
            std::move(executor), std::move(_body), std::move(_condition), std::move(_delay));
        // Launch the recursive chain using the helper class.
        return loop->run();
    }

private:
    /**
     * Helper class to perform the actual looping logic with a recursive member function run().
     * Mostly needed to clean up lambda captures and make the looping logic more readable.
     */
    struct TryUntilLoopWithDelay : public std::enable_shared_from_this<TryUntilLoopWithDelay> {
        TryUntilLoopWithDelay(std::shared_ptr<executor::TaskExecutor> executor,
                              BodyCallable executeLoopBody,
                              ConditionCallable shouldStopIteration,
                              Delay delay)
            : executor(std::move(executor)),
              executeLoopBody(std::move(executeLoopBody)),
              shouldStopIteration(std::move(shouldStopIteration)),
              delay(std::move(delay)) {}

        /**
         * Performs actual looping through recursion.
         */
        ExecutorFuture<FutureContinuationResult<BodyCallable>> run() {
            using ReturnType =
                typename decltype(getReturnType<decltype(executeLoopBody())>())::type;
            auto future = ExecutorFuture<void>(executor).then(executeLoopBody);

            return std::move(future).onCompletion(
                [this, self = this->shared_from_this()](StatusOrStatusWith<ReturnType> s) mutable {
                    if (shouldStopIteration(s))
                        return ExecutorFuture<ReturnType>(executor, std::move(s));

                    // Retry after a delay.
                    return sleepFor(executor, Milliseconds(delay)).then([this, self]() mutable {
                        return run();
                    });
                });
        }

        std::shared_ptr<executor::TaskExecutor> executor;
        BodyCallable executeLoopBody;
        ConditionCallable shouldStopIteration;
        Delay delay;
    };

    BodyCallable _body;
    ConditionCallable _condition;
    Delay _delay;
};

/**
 * Represents an intermediate state which holds the body and condition of a try-until loop.  See
 * comments for AsyncTry for usage.
 *
 * Note: This is a helper class and is not intended for standalone usage.
 */
template <typename BodyCallable, typename ConditionCallable>
class [[nodiscard]] AsyncTryUntil {
public:
    explicit AsyncTryUntil(BodyCallable && body, ConditionCallable && condition)
        : _body(std::move(body)), _condition(std::move(condition)) {}

    /**
     * Creates a delay which takes place after evaluating the condition and before executing the
     * loop body.
     */
    template <typename Delay>
    auto withDelayBetweenIterations(Delay delay)&& {
        return AsyncTryUntilWithDelay(std::move(_body), std::move(_condition), std::move(delay));
    }

    /**
     * Launches the loop and returns an ExecutorFuture that will be resolved when the loop is
     * complete.
     *
     * The returned ExecutorFuture contains the last result returned by the loop body. If the last
     * iteration of the loop body threw an exception or otherwise returned an error status, the
     * returned ExecutorFuture will contain that error.
     */
    auto on(std::shared_ptr<executor::TaskExecutor> executor)&& {
        auto loop = std::make_shared<TryUntilLoop>(
            std::move(executor), std::move(_body), std::move(_condition));
        // Launch the recursive chain using the helper class.
        return loop->run();
    }

private:
    /**
     * Helper class to perform the actual looping logic with a recursive member function run().
     * Mostly needed to clean up lambda captures and make the looping logic more readable.
     */
    struct TryUntilLoop : public std::enable_shared_from_this<TryUntilLoop> {
        TryUntilLoop(std::shared_ptr<executor::TaskExecutor> executor,
                     BodyCallable executeLoopBody,
                     ConditionCallable shouldStopIteration)
            : executor(std::move(executor)),
              executeLoopBody(std::move(executeLoopBody)),
              shouldStopIteration(std::move(shouldStopIteration)) {}

        /**
         * Performs actual looping through recursion.
         */
        ExecutorFuture<FutureContinuationResult<BodyCallable>> run() {
            using ReturnType =
                typename decltype(getReturnType<decltype(executeLoopBody())>())::type;
            auto future = ExecutorFuture<void>(executor).then(executeLoopBody);

            return std::move(future).onCompletion(
                [this, self = this->shared_from_this()](StatusOrStatusWith<ReturnType> s) mutable {
                    if (shouldStopIteration(s))
                        return ExecutorFuture<ReturnType>(executor, std::move(s));

                    return run();
                });
        }

        std::shared_ptr<executor::TaskExecutor> executor;
        BodyCallable executeLoopBody;
        ConditionCallable shouldStopIteration;
    };

    BodyCallable _body;
    ConditionCallable _condition;
};

}  // namespace future_util_details

/**
 * A fluent-style API for executing asynchronous, future-returning try-until loops.
 *
 * Example usage to send a request until a successful status is returned:
 *    ExecutorFuture<Response> response =
 *           AsyncTry([] { return sendRequest(); })
 *          .until([](StatusWith<Response> swResponse) { return swResponse.isOK(); })
 *          .withDelayBetweenIterations(Milliseconds(100)) // This call is optional.
 *          .on(executor);
 *
 * Note that the AsyncTry() call passes on the return value of its input lambda (the *body*) to the
 * condition lambda of Until, even if the body returns an error or throws - in which case the
 * StatusWith<T> will contain an error status. The delay inserted by WithDelayBetweenIterations
 * takes place after evaluating the condition and before executing the loop body an extra time.
 */
template <typename Callable>
class [[nodiscard]] AsyncTry {
public:
    explicit AsyncTry(Callable && callable) : _body(std::move(callable)) {}

    template <typename Condition>
    auto until(Condition && condition)&& {
        return future_util_details::AsyncTryUntil(std::move(_body), std::move(condition));
    }

    Callable _body;
};

}  // namespace mongo
