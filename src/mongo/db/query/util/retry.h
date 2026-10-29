// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_detail.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {

static constexpr size_t kDefaultMaxRetries = 10;

namespace detail {

/**
 * Shared implementation for retryOn() overloads.
 */
template <ErrorCodes::Error E, typename Fn, typename OnError>
auto retryOnImpl(std::string_view opName, Fn&& fn, OnError&& onError, size_t maxNumRetries) {
    size_t attempt = 0;

    while (true) {
        try {
            return fn();
        } catch (ExceptionFor<E>& ex) {
            if (attempt >= maxNumRetries) {
                ex.addContext(str::stream()
                              << "Exhausted max retries (" << maxNumRetries << ") for " << opName);
                throw;
            }

            onError(ex);

            logv2::detail::doLog(11486800,
                                 logv2::LogSeverity::Debug(1),
                                 {logv2::LogComponent::kQuery},
                                 "Retrying after retryable error",
                                 "errorCode"_attr = E,
                                 "attempt"_attr = attempt,
                                 "maxRetries"_attr = maxNumRetries,
                                 "reason"_attr = ex.reason());

            ++attempt;
        }
    }

    MONGO_UNREACHABLE;
}
}  // namespace detail

/**
 * Retries a callable up to `maxNumRetries` times if it throws ExceptionFor<E>. Returns the result
 * of the callable or propagates the exception once retries are exhausted.
 */
template <ErrorCodes::Error E, typename Fn>
auto retryOn(std::string_view opName, Fn&& fn, size_t maxNumRetries = kDefaultMaxRetries) {
    return detail::retryOnImpl<E>(
        opName, std::forward<Fn>(fn), [](const ExceptionFor<E>&) {}, maxNumRetries);
}

/**
 * Overload that also invokes `onError(ex)` after each retryable ExceptionFor<E>, allowing callers
 * to adjust state based on the exception before retrying.
 */
template <ErrorCodes::Error E, typename Fn, typename OnError>
auto retryOn(std::string_view opName, Fn&& fn, size_t maxNumRetries, OnError&& onError) {
    return detail::retryOnImpl<E>(
        opName, std::forward<Fn>(fn), std::forward<OnError>(onError), maxNumRetries);
}

/**
 * Stateful helper that retries a callable `fn(State&)` up to `maxNumRetries` times when it throws
 * ExceptionFor<E>. `onError` can update the state between attempts.
 */
template <ErrorCodes::Error E, typename State, typename Fn, typename OnError>
auto retryOnWithState(
    std::string_view opName, State initialState, size_t maxNumRetries, Fn&& fn, OnError&& onError) {
    State state = std::move(initialState);

    auto body = [&]() {
        return fn(state);
    };

    auto onErrorAdapter = [&](ExceptionFor<E>& ex) {
        onError(ex, state);
    };

    return retryOn<E>(opName, body, maxNumRetries, onErrorAdapter);
}

namespace detail {

/**
 * Helper struct to pair an error code (as template parameter) with its handler function.
 */
template <ErrorCodes::Error E, typename OnError>
struct ErrorHandler {
    OnError handler;
};

/**
 * Trait to detect if a type is an ErrorHandler.
 */
template <typename T>
struct IsErrorHandler : std::false_type {};
template <ErrorCodes::Error E, typename OnError>
struct IsErrorHandler<ErrorHandler<E, OnError>> : std::true_type {};
template <typename T>
constexpr bool isErrorHandler = IsErrorHandler<T>::value;

/**
 * Recursive helper to try handling an exception with any of the provided handlers.
 * Base case: no handlers left means that the exception was not handled and should be thrown.
 */
template <typename State>
bool tryHandleWithAny(State& state, size_t attempt, size_t maxNumRetries, std::string_view opName) {
    return false;
}

/**
 * Recursive helper to try handling an exception with any of the provided handlers.
 * Recursive case: try current handler, then recurse on the rest of the handlers if the exception
 * wasn't handled. This must be called from within a catch block.
 */
template <ErrorCodes::Error E, typename State, typename OnError, typename... RestHandlers>
bool tryHandleWithAny(State& state,
                      size_t attempt,
                      size_t maxNumRetries,
                      std::string_view opName,
                      ErrorHandler<E, OnError> errorHandler,
                      RestHandlers... rest) {
    try {
        throw;
    } catch (ExceptionFor<E>& ex) {
        if (attempt >= maxNumRetries) {
            ex.addContext(str::stream()
                          << "Exhausted max retries (" << maxNumRetries << ") for " << opName);
            throw;
        }

        errorHandler.handler(ex, state);

        logv2::detail::doLog(11637600,
                             logv2::LogSeverity::Debug(1),
                             {logv2::LogComponent::kQuery},
                             "Retrying after retryable error",
                             "errorCode"_attr = E,
                             "attempt"_attr = attempt,
                             "maxRetries"_attr = maxNumRetries,
                             "reason"_attr = ex.reason());
        return true;
    } catch (...) {
        return tryHandleWithAny(state, attempt, maxNumRetries, opName, rest...);
    }
}

/**
 * Main retry loop that handles multiple error codes.
 */
template <typename State, typename Fn, typename... ErrorHandlers>
auto retryOnWithStateMultiLoop(std::string_view opName,
                               State& state,
                               Fn&& fn,
                               size_t maxNumRetries,
                               ErrorHandlers... handlers) {
    size_t attempt = 0;
    while (true) {
        try {
            return fn(state);
        } catch (...) {
            if (tryHandleWithAny(state, attempt, maxNumRetries, opName, handlers...)) {
                ++attempt;
                continue;
            }
            throw;
        }
    }
    MONGO_UNREACHABLE;
}

}  // namespace detail

/**
 * Helper function to create an ErrorHandler with explicit error code and deduced handler type.
 */
template <ErrorCodes::Error E, typename OnError>
detail::ErrorHandler<E, std::decay_t<OnError>> makeErrorHandler(OnError&& handler) {
    return detail::ErrorHandler<E, std::decay_t<OnError>>{std::forward<OnError>(handler)};
}

/**
 * Overload that retries a callable `fn(State&)` up to `maxNumRetries` times when it throws any of
 * multiple ExceptionFor<E> types. Each error code has its own `onError` function that can update
 * the state between attempts.
 *
 * Usage:
 *   retryOnWithState(opName, initialState, maxRetries, fn,
 *       detail::ErrorHandler<ErrorCodes::E1>{onError1},
 *       detail::ErrorHandler<ErrorCodes::E2>{onError2},
 *       ...);
 */
template <typename State,
          typename Fn,
          typename... ErrorHandlers,
          typename = std::enable_if_t<(detail::isErrorHandler<ErrorHandlers> || ...)>>
auto retryOnWithState(std::string_view opName,
                      State initialState,
                      size_t maxNumRetries,
                      Fn&& fn,
                      ErrorHandlers... handlers) {
    State state = std::move(initialState);
    return detail::retryOnWithStateMultiLoop(
        opName, state, std::forward<Fn>(fn), maxNumRetries, handlers...);
}
}  // namespace mongo
