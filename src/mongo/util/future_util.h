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

#include "mongo/executor/task_executor.h"
#include "mongo/util/future.h"
#include "mongo/util/static_immortal.h"

namespace mongo {

/**
 * Returns a future which will be fulfilled at the given date.
 */
ExecutorFuture<void> sleepUntil(std::shared_ptr<executor::TaskExecutor> executor,
                                const Date_t& date);
/**
 * Returns a future which will be fulfilled after the given duration.
 */
ExecutorFuture<void> sleepFor(std::shared_ptr<executor::TaskExecutor> executor,
                              Milliseconds duration);

namespace future_util_details {

/**
 * Error status to use if any AsyncTry loop has been canceled.
 */
inline Status asyncTryCanceledStatus() {
    static StaticImmortal s = Status{ErrorCodes::CallbackCanceled, "AsyncTry loop canceled"};
    return *s;
}

/**
 * Creates an ExecutorFuture from the result of the input callable.
 */
template <typename Callable>
auto makeExecutorFutureWith(ExecutorPtr executor, Callable&& callable) {
    using CallableResult = std::invoke_result_t<Callable>;

    if constexpr (future_details::isFutureLike<CallableResult>) {
        try {
            return callable().thenRunOn(executor);
        } catch (const DBException& e) {
            return ExecutorFuture<FutureContinuationResult<Callable>>(executor, e.toStatus());
        }
    } else {
        return makeReadyFutureWith(callable).thenRunOn(executor);
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
     * complete. If the executor is already shut down or the cancelToken has already been canceled
     * before the loop is launched, the loop body will never run and the resulting ExecutorFuture
     * will be set with either a ShutdownInProgress or CallbackCanceled error.
     *
     * The returned ExecutorFuture contains the last result returned by the loop body. If the last
     * iteration of the loop body threw an exception or otherwise returned an error status, the
     * returned ExecutorFuture will contain that error.
     */
    auto on(std::shared_ptr<executor::TaskExecutor> executor, CancelationToken cancelToken)&& {
        auto loop = std::make_shared<TryUntilLoopWithDelay>(std::move(executor),
                                                            std::move(_body),
                                                            std::move(_condition),
                                                            std::move(_delay),
                                                            std::move(cancelToken));
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
                              Delay delay,
                              CancelationToken cancelToken)
            : executor(std::move(executor)),
              executeLoopBody(std::move(executeLoopBody)),
              shouldStopIteration(std::move(shouldStopIteration)),
              delay(std::move(delay)),
              cancelToken(std::move(cancelToken)) {}

        /**
         * Performs actual looping through recursion.
         */
        ExecutorFuture<FutureContinuationResult<BodyCallable>> run() {
            using ReturnType = FutureContinuationResult<BodyCallable>;

            // If the request is already canceled, don't run anything.
            if (cancelToken.isCanceled())
                return ExecutorFuture<ReturnType>(executor, asyncTryCanceledStatus());

            auto [promise, future] = makePromiseFuture<ReturnType>();

            // Kick off the asynchronous loop.
            runImpl(std::move(promise));

            return std::move(future).thenRunOn(executor);
        }


        /**
         * Helper function that schedules an asynchronous task. This task executes the loop body and
         * either terminates the loop by emplacing the resultPromise, or makes a recursive call to
         * reschedule another iteration of the loop.
         */
        template <typename ReturnType>
        void runImpl(Promise<ReturnType> resultPromise) {
            executor->schedule([this,
                                self = this->shared_from_this(),
                                resultPromise =
                                    std::move(resultPromise)](Status scheduleStatus) mutable {
                if (!scheduleStatus.isOK()) {
                    resultPromise.setError(std::move(scheduleStatus));
                    return;
                }

                using BodyCallableResult = std::invoke_result_t<BodyCallable>;
                // Convert the result of the loop body into an ExecutorFuture, even if the
                // loop body is not future-returning. This isn't strictly necessary but it
                // makes implementation easier.
                makeExecutorFutureWith(executor, executeLoopBody)
                    .getAsync([this, self, resultPromise = std::move(resultPromise)](
                                  StatusOrStatusWith<ReturnType>&& swResult) mutable {
                        if (cancelToken.isCanceled()) {
                            resultPromise.setError(asyncTryCanceledStatus());
                        } else if (shouldStopIteration(swResult)) {
                            resultPromise.setFrom(std::move(swResult));
                        } else {
                            // Retry after a delay.
                            executor->sleepFor(delay.getNext(), cancelToken)
                                .getAsync([this, self, resultPromise = std::move(resultPromise)](
                                              Status s) mutable {
                                    if (s.isOK()) {
                                        runImpl(std::move(resultPromise));
                                    }
                                });
                        }
                    });
            });
        }

        std::shared_ptr<executor::TaskExecutor> executor;
        BodyCallable executeLoopBody;
        ConditionCallable shouldStopIteration;
        Delay delay;
        CancelationToken cancelToken;
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
    template <typename DurationType>
    auto withDelayBetweenIterations(DurationType delay)&& {
        return AsyncTryUntilWithDelay(
            std::move(_body), std::move(_condition), ConstDelay<DurationType>(std::move(delay)));
    }

    /**
     * Creates an exponential delay which takes place after evaluating the condition and before
     * executing the loop body.
     */
    template <typename BackoffType>
    auto withBackoffBetweenIterations(BackoffType backoff)&& {
        return AsyncTryUntilWithDelay(
            std::move(_body), std::move(_condition), BackoffDelay<BackoffType>(std::move(backoff)));
    }

    /**
     * Launches the loop and returns an ExecutorFuture that will be resolved when the loop is
     * complete. If the executor is already shut down or the cancelToken has already been canceled
     * before the loop is launched, the loop body will never run and the resulting ExecutorFuture
     * will be set with either a ShutdownInProgress or CallbackCanceled error.
     *
     * The returned ExecutorFuture contains the last result returned by the loop body. If the last
     * iteration of the loop body threw an exception or otherwise returned an error status, the
     * returned ExecutorFuture will contain that error.
     */
    auto on(std::shared_ptr<executor::TaskExecutor> executor, CancelationToken cancelToken)&& {
        auto loop = std::make_shared<TryUntilLoop>(
            std::move(executor), std::move(_body), std::move(_condition), std::move(cancelToken));
        // Launch the recursive chain using the helper class.
        return loop->run();
    }

private:
    template <typename DurationType>
    class ConstDelay {
    public:
        explicit ConstDelay(DurationType delay) : _delay(delay) {}

        Milliseconds getNext() {
            return Milliseconds(_delay);
        }

    private:
        DurationType _delay;
    };

    template <typename BackoffType>
    class BackoffDelay {
    public:
        explicit BackoffDelay(BackoffType backoff) : _backoff(backoff) {}

        Milliseconds getNext() {
            return _backoff.nextSleep();
        }

    private:
        BackoffType _backoff;
    };

    /**
     * Helper class to perform the actual looping logic with a recursive member function run().
     * Mostly needed to clean up lambda captures and make the looping logic more readable.
     */
    struct TryUntilLoop : public std::enable_shared_from_this<TryUntilLoop> {
        TryUntilLoop(std::shared_ptr<executor::TaskExecutor> executor,
                     BodyCallable executeLoopBody,
                     ConditionCallable shouldStopIteration,
                     CancelationToken cancelToken)
            : executor(std::move(executor)),
              executeLoopBody(std::move(executeLoopBody)),
              shouldStopIteration(std::move(shouldStopIteration)),
              cancelToken(std::move(cancelToken)) {}

        /**
         * Performs actual looping through recursion.
         */
        ExecutorFuture<FutureContinuationResult<BodyCallable>> run() {
            using ReturnType = FutureContinuationResult<BodyCallable>;

            // If the request is already canceled, don't run anything.
            if (cancelToken.isCanceled())
                return ExecutorFuture<ReturnType>(executor, asyncTryCanceledStatus());

            auto [promise, future] = makePromiseFuture<ReturnType>();

            // Kick off the asynchronous loop.
            runImpl(std::move(promise));

            return std::move(future).thenRunOn(executor);
        }

        /**
         * Helper function that schedules an asynchronous task. This task executes the loop body and
         * either terminates the loop by emplacing the resultPromise, or makes a recursive call to
         * reschedule another iteration of the loop.
         */
        template <typename ReturnType>
        void runImpl(Promise<ReturnType> resultPromise) {
            executor->schedule(
                [this, self = this->shared_from_this(), resultPromise = std::move(resultPromise)](
                    Status scheduleStatus) mutable {
                    if (!scheduleStatus.isOK()) {
                        resultPromise.setError(std::move(scheduleStatus));
                        return;
                    }

                    // Convert the result of the loop body into an ExecutorFuture, even if the
                    // loop body is not Future-returning. This isn't strictly necessary but it
                    // makes implementation easier.
                    makeExecutorFutureWith(executor, executeLoopBody)
                        .getAsync([this, self, resultPromise = std::move(resultPromise)](
                                      StatusOrStatusWith<ReturnType>&& swResult) mutable {
                            if (cancelToken.isCanceled()) {
                                resultPromise.setError(asyncTryCanceledStatus());
                            } else if (shouldStopIteration(swResult)) {
                                resultPromise.setFrom(std::move(swResult));
                            } else {
                                runImpl(std::move(resultPromise));
                            }
                        });
                });
        }

        std::shared_ptr<executor::TaskExecutor> executor;
        BodyCallable executeLoopBody;
        ConditionCallable shouldStopIteration;
        CancelationToken cancelToken;
    };

    BodyCallable _body;
    ConditionCallable _condition;
};

// Helpers for functions which only take Future or ExecutorFutures, but not SemiFutures or
// SharedSemiFutures.
template <typename T>
inline constexpr bool isFutureOrExecutorFuture = false;
template <typename T>
inline constexpr bool isFutureOrExecutorFuture<Future<T>> = true;
template <typename T>
inline constexpr bool isFutureOrExecutorFuture<ExecutorFuture<T>> = true;

static inline const std::string kWhenAllSucceedEmptyInputInvariantMsg =
    "Must pass at least one future to whenAllSucceed";

/**
 * Turns a variadic parameter pack into a vector without invoking copies if possible via static
 * casts.
 */
template <typename... U, typename T = std::common_type_t<U...>>
std::vector<T> variadicArgsToVector(U&&... elems) {
    std::vector<T> vector;
    vector.reserve(sizeof...(elems));
    (vector.push_back(std::forward<U>(elems)), ...);
    return vector;
}

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

/**
 * For an input vector of Future<T> or ExecutorFuture<T> elements, returns a
 * SemiFuture<std::vector<T>> that will be resolved when all input futures succeed or set with an
 * error as soon as any input future is set with an error. The resulting vector contains the results
 * of all of the input futures in the same order in which they were provided.
 */
TEMPLATE(typename FutureLike,
         typename Value = typename FutureLike::value_type,
         typename ResultVector = std::vector<Value>)
REQUIRES(!std::is_void_v<Value> && future_util_details::isFutureOrExecutorFuture<FutureLike>)
SemiFuture<ResultVector> whenAllSucceed(std::vector<FutureLike>&& futures) {
    invariant(futures.size() > 0, future_util_details::kWhenAllSucceedEmptyInputInvariantMsg);

    // A structure used to share state between the input futures.
    struct SharedBlock {
        SharedBlock(size_t numFuturesToWaitFor, Promise<ResultVector> result)
            : numFuturesToWaitFor(numFuturesToWaitFor),
              resultPromise(std::move(result)),
              intermediateResult(numFuturesToWaitFor) {}
        // Total number of input futures.
        const size_t numFuturesToWaitFor;
        // Tracks the number of input futures which have resolved with success so far.
        AtomicWord<size_t> numResultsReturnedWithSuccess{0};
        // Tracks whether or not the resultPromise has been set. Only used for the error case.
        AtomicWord<bool> completedWithError{false};
        // The promise corresponding to the resulting SemiFuture returned by this function.
        Promise<ResultVector> resultPromise;
        // A vector containing the results of each input future.
        ResultVector intermediateResult;
    };

    auto [promise, future] = makePromiseFuture<ResultVector>();
    auto sharedBlock = std::make_shared<SharedBlock>(futures.size(), std::move(promise));

    for (size_t i = 0; i < futures.size(); ++i) {
        std::move(futures[i]).getAsync([sharedBlock, myIndex = i](StatusWith<Value> swValue) {
            if (swValue.isOK()) {
                // Best effort check that no error has returned, not required for correctness.
                if (!sharedBlock->completedWithError.loadRelaxed()) {
                    // Put this result in its proper slot in the output vector.
                    sharedBlock->intermediateResult[myIndex] = std::move(swValue.getValue());
                    auto numResultsReturnedWithSuccess =
                        sharedBlock->numResultsReturnedWithSuccess.addAndFetch(1);
                    // If this is the last result to return, set the promise. Note that this
                    // will never be true if one of the input futures resolves with an error,
                    // since the future with an error will not cause the
                    // numResultsReturnedWithSuccess count to be incremented.
                    if (numResultsReturnedWithSuccess == sharedBlock->numFuturesToWaitFor) {
                        // All results are ready.
                        sharedBlock->resultPromise.emplaceValue(
                            std::move(sharedBlock->intermediateResult));
                    }
                }
            } else {
                // Make sure no other error has already been set before setting the promise.
                if (!sharedBlock->completedWithError.swap(true)) {
                    sharedBlock->resultPromise.setError(std::move(swValue.getStatus()));
                }
            }
        });
    }

    return std::move(future).semi();
}

/**
 * Variant of whenAllSucceed for void input futures. The only behavior difference is that it returns
 * SemiFuture<void> instead of SemiFuture<std::vector<T>>.
 */
TEMPLATE(typename FutureLike, typename Value = typename FutureLike::value_type)
REQUIRES(std::is_void_v<Value>&& future_util_details::isFutureOrExecutorFuture<FutureLike>)
SemiFuture<void> whenAllSucceed(std::vector<FutureLike>&& futures) {
    invariant(futures.size() > 0, future_util_details::kWhenAllSucceedEmptyInputInvariantMsg);

    // A structure used to share state between the input futures.
    struct SharedBlock {
        SharedBlock(size_t numFuturesToWaitFor, Promise<void> result)
            : numFuturesToWaitFor(numFuturesToWaitFor), resultPromise(std::move(result)) {}
        // Total number of input futures.
        const size_t numFuturesToWaitFor;
        // Tracks the number of input futures which have resolved with success so far.
        AtomicWord<size_t> numResultsReturnedWithSuccess{0};
        // Tracks whether or not the resultPromise has been set. Only used for the error case.
        AtomicWord<bool> completedWithError{false};
        // The promise corresponding to the resulting SemiFuture returned by this function.
        Promise<void> resultPromise;
    };

    auto [promise, future] = makePromiseFuture<void>();
    auto sharedBlock = std::make_shared<SharedBlock>(futures.size(), std::move(promise));

    for (size_t i = 0; i < futures.size(); ++i) {
        std::move(futures[i]).getAsync([sharedBlock](Status status) {
            if (status.isOK()) {
                // Best effort check that no error has returned, not required for correctness
                if (!sharedBlock->completedWithError.loadRelaxed()) {
                    auto numResultsReturnedWithSuccess =
                        sharedBlock->numResultsReturnedWithSuccess.addAndFetch(1);
                    // If this is the last result to return, set the promise. Note that this will
                    // never be true if one of the input futures resolves with an error, since the
                    // future with an error will not cause the numResultsReturnedWithSuccess count
                    // to be incremented.
                    if (numResultsReturnedWithSuccess == sharedBlock->numFuturesToWaitFor) {
                        // All results are ready.
                        sharedBlock->resultPromise.emplaceValue();
                    }
                }
            } else {
                // Make sure no other error has already been set before setting the promise.
                if (!sharedBlock->completedWithError.swap(true)) {
                    sharedBlock->resultPromise.setError(std::move(status));
                }
            }
        });
    }

    return std::move(future).semi();
}

/**
 * Given a vector of input Futures or ExecutorFutures, returns a SemiFuture that contains the
 * results of each input future wrapped in a StatusWith to indicate whether it resolved with success
 * or failure and will be resolved when all of the input futures have resolved.
 */
template <typename FutureT,
          typename Value = typename FutureT::value_type,
          typename ResultVector = std::vector<StatusOrStatusWith<Value>>>
SemiFuture<ResultVector> whenAll(std::vector<FutureT>&& futures) {
    invariant(futures.size() > 0);

    /**
     * A structure used to share state between the input futures.
     */
    struct SharedBlock {
        SharedBlock(size_t numFuturesToWaitFor, Promise<ResultVector> result)
            : numFuturesToWaitFor(numFuturesToWaitFor),
              intermediateResult(numFuturesToWaitFor, {ErrorCodes::InternalError, ""}),
              resultPromise(std::move(result)) {}
        // Total number of input futures.
        const size_t numFuturesToWaitFor;
        // Tracks the number of input futures which have resolved so far.
        AtomicWord<size_t> numReady{0};
        // A vector containing the results of each input future.
        ResultVector intermediateResult;
        // The promise corresponding to the resulting SemiFuture returned by this function.
        Promise<ResultVector> resultPromise;
    };

    auto [promise, future] = makePromiseFuture<ResultVector>();
    auto sharedBlock = std::make_shared<SharedBlock>(futures.size(), std::move(promise));

    for (size_t i = 0; i < futures.size(); ++i) {
        std::move(futures[i]).getAsync([sharedBlock, myIndex = i](StatusOrStatusWith<Value> value) {
            sharedBlock->intermediateResult[myIndex] = std::move(value);

            auto numReady = sharedBlock->numReady.addAndFetch(1);
            invariant(numReady <= sharedBlock->numFuturesToWaitFor);

            if (numReady == sharedBlock->numFuturesToWaitFor) {
                // All results are ready.
                sharedBlock->resultPromise.emplaceValue(std::move(sharedBlock->intermediateResult));
            }
        });
    }

    return std::move(future).semi();
}

/**
 * Result type for the whenAny function.
 */
template <typename T>
struct WhenAnyResult {
    // The result of the future that resolved first.
    StatusOrStatusWith<T> result;
    // The index of the future that resolved first.
    size_t index;
};

/**
 * Given a vector of input Futures or ExecutorFutures, returns a SemiFuture which will contain a
 * struct containing the first of those futures to resolve along with its index in the input array.
 */
template <typename FutureT,
          typename Value = typename FutureT::value_type,
          typename Result = WhenAnyResult<Value>>
SemiFuture<Result> whenAny(std::vector<FutureT>&& futures) {
    invariant(futures.size() > 0);

    /**
     * A structure used to share state between the input futures.
     */
    struct SharedBlock {
        SharedBlock(Promise<Result> result) : resultPromise(std::move(result)) {}
        // Tracks whether or not the resultPromise has been set.
        AtomicWord<bool> done{false};
        // The promise corresponding to the resulting SemiFuture returned by this function.
        Promise<Result> resultPromise;
    };

    auto [promise, future] = makePromiseFuture<Result>();
    auto sharedBlock = std::make_shared<SharedBlock>(std::move(promise));

    for (size_t i = 0; i < futures.size(); ++i) {
        std::move(futures[i]).getAsync([sharedBlock, myIndex = i](StatusOrStatusWith<Value> value) {
            // If this is the first input future to complete, change done to true and set the
            // value on the promise.
            if (!sharedBlock->done.swap(true)) {
                sharedBlock->resultPromise.emplaceValue(Result{std::move(value), myIndex});
            }
        });
    }

    return std::move(future).semi();
}

/**
 * Variadic template overloads for the above helper functions. Though not strictly necessary,
 * we peel off the first element of each input list in order to assist the compiler in type
 * inference and to prevent 0 length lists from compiling.
 */
TEMPLATE(typename... FuturePack,
         typename FutureLike = std::common_type_t<FuturePack...>,
         typename Value = typename FutureLike::value_type,
         typename ResultVector = std::vector<Value>)
REQUIRES(future_util_details::isFutureOrExecutorFuture<FutureLike>)
auto whenAllSucceed(FuturePack&&... futures) {
    return whenAllSucceed(
        future_util_details::variadicArgsToVector(std::forward<FuturePack>(futures)...));
}

template <typename... FuturePack,
          typename FutureT = std::common_type_t<FuturePack...>,
          typename Value = typename FutureT::value_type,
          typename ResultVector = std::vector<StatusOrStatusWith<Value>>>
SemiFuture<ResultVector> whenAll(FuturePack&&... futures) {
    return whenAll(future_util_details::variadicArgsToVector(std::forward<FuturePack>(futures)...));
}

template <typename... FuturePack,
          typename FutureT = std::common_type_t<FuturePack...>,
          typename Result = WhenAnyResult<typename FutureT::value_type>>
SemiFuture<Result> whenAny(FuturePack&&... futures) {
    return whenAny(future_util_details::variadicArgsToVector(std::forward<FuturePack>(futures)...));
}

namespace future_util {

/**
 * This class is a helper for ensuring RAII-like behavior in async code.
 *
 * This class constructs a heap allocated State object in make(), and then uses that State object
 * with a Launcher functor to create a future. The State object is persisted until the future
 * created by the Launcher completes. If the Launcher emits an exception, the state is destructed.
 * If the State ctor throws, then the error Status is returned as a ready future instead of invoking
 * the Launcher. The simplest example would be a guard that is destroyed once an async function
 * finishes:
 * ```
 * auto myFuture = future_util::AsyncState::make<MyGuard>({})
 *     .thenWithState([](MyGuard*) { return myAsyncFunc(); });
 * ```
 *
 * Note that this class is not usable for ExecutorFutures because there is no tapAll() function to
 * invoke. If you want similar behavior, simply bind your state to the final callback in the async
 * chain, but do be mindful of TODO(SERVER-52942).
 */
template <typename State>
class [[nodiscard]] AsyncState {
public:
    explicit AsyncState(std::unique_ptr<State> state)
        : _status(Status::OK()), _state(std::move(state)) {}

    /**
     * Consume the AsyncState and bind the underlying state to the tail of the future returned from
     * the launcher functor.
     *
     * If the AsyncState was constructed with a Status, then return the captured status instead of
     * running the launcher.
     */
    template <typename Launcher>
        auto thenWithState(Launcher && launcher) && noexcept {
        using namespace future_details;
        using ReturnType = FutureFor<NormalizedCallResult<Launcher, State*>>;

        if (!_status.isOK()) {
            // The factory threw, we have no state to run the launcher with.
            return ReturnType::makeReady(std::move(_status));
        }

        auto ptr = _state.get();
        return makeReadyFutureWith([&] {
            auto future = coerceToFuture(std::forward<Launcher>(launcher)(ptr));
            return std::move(future).tapAll([state = std::move(_state)](auto&&) mutable {
                // Finally, release our state.
                auto localState = std::move(state);
            });
        });
    }

    /**
     * This function is a helper for making an AsyncState given ala make_unique.
     *
     * If an exception would be emitted, it is instead stored in the AsyncState.
     */
    template <typename... Args>
    static auto make(Args && ... args) noexcept {
        try {
            auto ptr = std::make_unique<State>(std::forward<Args>(args)...);
            return AsyncState(std::move(ptr));
        } catch (const DBException& ex) {
            return AsyncState(ex.toStatus());
        }
    }

private:
    /**
     * This ctor allows make to capture exceptions and defer them until someone invokes
     * thenWithState(). It is private simply because it is difficult to justify a good use case for
     * it by an external user.
     */
    explicit AsyncState(Status status) : _status(std::move(status)) {}

    Status _status;
    std::unique_ptr<State> _state;
};

// This function is syntactic sugar for AsyncState<T>::make().
template <typename T, typename... Args>
auto makeState(Args&&... args) {
    return AsyncState<T>::make(std::forward<Args>(args)...);
}

}  // namespace future_util

}  // namespace mongo
