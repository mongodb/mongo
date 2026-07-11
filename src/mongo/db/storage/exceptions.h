// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <string>
#include <string_view>

#include <fmt/format.h>

namespace [[MONGO_MOD_PUBLIC]] mongo {
/**
 * A faster alternative to `iasserted`, designed to throw exceptions for unexceptional events on the
 * critical execution path (e.g., `WriteConflict`).
 */
template <ErrorCodes::Error ec>
[[MONGO_MOD_FILE_PRIVATE]] [[noreturn]] void throwExceptionFor(std::string reason) {
    throw ExceptionFor<ec>({ec, std::move(reason)});
}

/**
 * A `WriteConflictException` is thrown if during a write, two or more operations conflict with each
 * other. For example if two operations get the same version of a document, and then both try to
 * modify that document, this exception will get thrown by one of them.
 */
[[noreturn]] inline void throwWriteConflictException(std::string_view context) {
    throwExceptionFor<ErrorCodes::WriteConflict>(fmt::format(
        "Caused by :: {} :: Please retry your operation or multi-document transaction.", context));
}

/**
 * A `TemporarilyUnavailableException` is thrown if an operation aborts due to the server being
 * temporarily unavailable, e.g. due to excessive load. For user-originating operations, this will
 * be retried internally by the `writeConflictRetry` helper a finite number of times before
 * eventually being returned.
 */
[[noreturn]] inline void throwTemporarilyUnavailableException(std::string context) {
    throwExceptionFor<ErrorCodes::TemporarilyUnavailable>(std::move(context));
}

/**
 * `WriteConflictRetryLimitExceeded` is thrown by `PlanExecutorImpl::_handleNeedYield()` when an
 * operation exceeds its adaptive cap on consecutive WriteConflictException retries. Tagged
 * `[SystemOverloadedError, RetriableError]` so retryable-writes-aware drivers auto-backoff and
 * retry the op. Clients MUST retry with backoff: zero-delay retry re-enters the limit and creates
 * a positive feedback loop. See `internalQueryWriteConflictRetryLimitMax`.
 */
[[noreturn]] inline void throwWriteConflictRetryLimitExceededException(std::string context) {
    throwExceptionFor<ErrorCodes::WriteConflictRetryLimitExceeded>(std::move(context));
}

/**
 * A `TransactionTooLargeForCache` is thrown if it has been determined that it is unlikely to
 * ever complete the operation because the configured cache is insufficient to hold all the
 * transaction state. This helps to avoid retrying, maybe indefinitely, a transaction which would
 * never be able to complete.
 */
[[noreturn]] inline void throwTransactionTooLargeForCache(std::string context) {
    throwExceptionFor<ErrorCodes::TransactionTooLargeForCache>(std::move(context));
}

}  // namespace mongo
