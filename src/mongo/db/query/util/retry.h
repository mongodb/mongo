/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_detail.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

namespace mongo {

static constexpr size_t kDefaultMaxRetries = 10;

namespace detail {

/**
 * Shared implementation for retryOn() overloads.
 */
template <ErrorCodes::Error E, typename Fn, typename OnError>
auto retryOnImpl(StringData opName, Fn&& fn, OnError&& onError, size_t maxNumRetries) {
    for (size_t attempt = 0; attempt <= maxNumRetries; ++attempt) {
        try {
            return fn();
        } catch (ExceptionFor<E>& ex) {
            if (attempt == maxNumRetries) {
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
auto retryOn(StringData opName, Fn&& fn, size_t maxNumRetries = kDefaultMaxRetries) {
    return detail::retryOnImpl<E>(
        opName, std::forward<Fn>(fn), [](const ExceptionFor<E>&) {}, maxNumRetries);
}

/**
 * Overload that also invokes `onError(ex)` after each retryable ExceptionFor<E>, allowing callers
 * to adjust state based on the exception before retrying.
 */
template <ErrorCodes::Error E, typename Fn, typename OnError>
auto retryOn(StringData opName, Fn&& fn, size_t maxNumRetries, OnError&& onError) {
    return detail::retryOnImpl<E>(
        opName, std::forward<Fn>(fn), std::forward<OnError>(onError), maxNumRetries);
}

/**
 * Stateful helper that retries a callable `fn(State&)` up to `maxNumRetries` times when it throws
 * ExceptionFor<E>. `onError` can update the state between attempts.
 */
template <ErrorCodes::Error E, typename State, typename Fn, typename OnError>
auto retryOnWithState(
    StringData opName, State initialState, size_t maxNumRetries, Fn&& fn, OnError&& onError) {
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
bool tryHandleWithAny(State& state, size_t attempt, size_t maxNumRetries, StringData opName) {
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
                      StringData opName,
                      ErrorHandler<E, OnError> errorHandler,
                      RestHandlers... rest) {
    try {
        throw;
    } catch (ExceptionFor<E>& ex) {
        if (attempt == maxNumRetries) {
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
auto retryOnWithStateMultiLoop(
    StringData opName, State& state, Fn&& fn, size_t maxNumRetries, ErrorHandlers... handlers) {
    for (size_t attempt = 0; attempt <= maxNumRetries; ++attempt) {
        try {
            return fn(state);
        } catch (...) {
            if (tryHandleWithAny(state, attempt, maxNumRetries, opName, handlers...)) {
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
auto retryOnWithState(StringData opName,
                      State initialState,
                      size_t maxNumRetries,
                      Fn&& fn,
                      ErrorHandlers... handlers) {
    State state = std::move(initialState);
    return detail::retryOnWithStateMultiLoop(
        opName, state, std::forward<Fn>(fn), maxNumRetries, handlers...);
}
}  // namespace mongo
