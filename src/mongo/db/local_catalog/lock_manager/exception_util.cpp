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

#include "mongo/db/local_catalog/lock_manager/exception_util.h"

#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/local_catalog/lock_manager/exception_util_gen.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/storage/exceptions.h"
#include "mongo/db/storage/execution_context.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/log_and_backoff.h"

#include <cstddef>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {
namespace {
auto& temporarilyUnavailableErrors =
    *MetricBuilder<Counter64>{"operation.temporarilyUnavailableErrors"};
auto& temporarilyUnavailableErrorsEscaped =
    *MetricBuilder<Counter64>{"operation.temporarilyUnavailableErrorsEscaped"};
auto& temporarilyUnavailableErrorsConvertedToWriteConflict =
    *MetricBuilder<Counter64>{"operation.temporarilyUnavailableErrorsConvertedToWriteConflict"};

auto& transactionTooLargeForCacheErrors =
    *MetricBuilder<Counter64>{"operation.transactionTooLargeForCacheErrors"};
auto& transactionTooLargeForCacheErrorsConvertedToWriteConflict = *MetricBuilder<Counter64>{
    "operation.transactionTooLargeForCacheErrorsConvertedToWriteConflict"};

/** Apply floating point multiply to a Duration. */
template <typename Dur>
Dur floatScaleDuration(double scale, Dur dur) {
    return Dur{static_cast<typename Dur::rep>(scale * durationCount<Dur>(dur))};
}

void handleTransactionTooLargeForCacheException(OperationContext* opCtx,
                                                RecoveryUnit& ru,
                                                StringData opStr,
                                                const NamespaceStringOrUUID& nssOrUUID,
                                                const Status& s,
                                                size_t writeConflictAttempts) {
    transactionTooLargeForCacheErrors.increment(1);
    if (opCtx->writesAreReplicated()) {
        // Surface error on primaries.
        throw;
    }
    // If an operation succeeds on primary, it should always be retried on secondaries. Secondaries
    // always retry TemporarilyUnavailableExceptions and WriteConflictExceptions indefinitely, the
    // only difference being the rate of retry. We prefer retrying faster, by converting to
    // WriteConflictException, to avoid stalling replication longer than necessary.
    transactionTooLargeForCacheErrorsConvertedToWriteConflict.increment(1);

    // Handle as write conflict.
    logAndRecordWriteConflictAndBackoff(
        opCtx, writeConflictAttempts, opStr, s.reason(), NamespaceStringOrUUID(nssOrUUID));
    ru.abandonSnapshot();
}
}  // namespace

MONGO_FAIL_POINT_DEFINE(skipWriteConflictRetries);

void recordWriteConflict(OperationContext* opCtx, int64_t n) {
    invariant(n > 0);
    StorageExecutionContext::get(opCtx)->getStorageMetrics().incrementWriteConflicts(n);
}

void recordTemporarilyUnavailableErrors(OperationContext* opCtx, int64_t n) {
    invariant(n > 0);
    StorageExecutionContext::get(opCtx)->getStorageMetrics().incrementTemporarilyUnavailableErrors(
        n);
}

void logWriteConflictAndBackoff(size_t attempt,
                                StringData operation,
                                StringData reason,
                                const NamespaceStringOrUUID& nssOrUUID) {
    auto severity = ((attempt != 0) && ((attempt % 1000) == 0)) ? logv2::LogSeverity::Info()
                                                                : logv2::LogSeverity::Debug(1);

    logAndBackoff(4640401,
                  logv2::LogComponent::kWrite,
                  severity,
                  static_cast<size_t>(attempt),
                  "Caught WriteConflictException",
                  "operation"_attr = operation,
                  "reason"_attr = reason,
                  "namespace"_attr = toStringForLogging(nssOrUUID));
}

void logAndRecordWriteConflictAndBackoff(OperationContext* opCtx,
                                         size_t attempt,
                                         StringData operation,
                                         StringData reason,
                                         const NamespaceStringOrUUID& nssOrUUID) {
    recordWriteConflict(opCtx);
    logWriteConflictAndBackoff(attempt, operation, reason, nssOrUUID);
}

void handleTemporarilyUnavailableException(OperationContext* opCtx,
                                           size_t tempUnavailAttempts,
                                           StringData opStr,
                                           const NamespaceStringOrUUID& nssOrUUID,
                                           const Status& s,
                                           size_t& writeConflictAttempts) {
    handleTemporarilyUnavailableException(opCtx,
                                          *shard_role_details::getRecoveryUnit(opCtx),
                                          tempUnavailAttempts,
                                          opStr,
                                          nssOrUUID,
                                          s,
                                          writeConflictAttempts);
}
void handleTemporarilyUnavailableException(OperationContext* opCtx,
                                           RecoveryUnit& ru,
                                           size_t tempUnavailAttempts,
                                           StringData opStr,
                                           const NamespaceStringOrUUID& nssOrUUID,
                                           const Status& s,
                                           size_t& writeConflictAttempts) {
    recordTemporarilyUnavailableErrors(opCtx);

    ru.abandonSnapshot();
    temporarilyUnavailableErrors.increment(1);

    // Internal operations cannot escape a TUE to the client. Convert it to a write conflict
    // exception and handle it accordingly.
    if (!opCtx->getClient()->isFromUserConnection()) {
        temporarilyUnavailableErrorsConvertedToWriteConflict.increment(1);
        logAndRecordWriteConflictAndBackoff(
            opCtx, writeConflictAttempts, opStr, s.reason(), NamespaceStringOrUUID(nssOrUUID));
        ++writeConflictAttempts;
        return;
    }

    invariant(opCtx->getClient()->isFromUserConnection());

    if (tempUnavailAttempts >
        static_cast<size_t>(gTemporarilyUnavailableExceptionMaxRetryAttempts.load())) {
        LOGV2_DEBUG(6083901,
                    1,
                    "Too many TemporarilyUnavailableException's, giving up",
                    "reason"_attr = s.reason(),
                    "attempts"_attr = tempUnavailAttempts,
                    "operation"_attr = opStr,
                    "namespace"_attr = toStringForLogging(nssOrUUID));
        temporarilyUnavailableErrorsEscaped.increment(1);
        throw ExceptionFor<ErrorCodes::TemporarilyUnavailable>(s);
    }

    // Back off linearly with the retry attempt number.
    auto sleepFor = Milliseconds(gTemporarilyUnavailableExceptionRetryBackoffBaseMs.load()) *
        static_cast<int64_t>(tempUnavailAttempts);
    LOGV2_DEBUG(6083900,
                1,
                "Caught TemporarilyUnavailableException",
                "reason"_attr = s.reason(),
                "attempts"_attr = tempUnavailAttempts,
                "operation"_attr = opStr,
                "sleepFor"_attr = sleepFor,
                "namespace"_attr = toStringForLogging(nssOrUUID));
    opCtx->sleepFor(sleepFor);
}

void convertToWCEAndRethrow(OperationContext* opCtx,
                            StringData opStr,
                            const ExceptionFor<ErrorCodes::TemporarilyUnavailable>& e) {
    // For multi-document transactions, since WriteConflicts are tagged as
    // TransientTransactionErrors and TemporarilyUnavailable errors are not, convert the error to a
    // WriteConflict to allow users of multi-document transactions to retry without changing
    // any behavior.
    temporarilyUnavailableErrorsConvertedToWriteConflict.increment(1);
    recordWriteConflict(opCtx);
    throwWriteConflictException(e.reason());
}

void WriteConflictRetryAlgorithm::_emitLog(StringData reason) {
    logv2::detail::doLog(46404,
                         _logSeverity(),
                         {logv2::LogComponent::kWrite},
                         "Caught WriteConflictException",
                         "operation"_attr = _opStr,
                         "reason"_attr = reason,
                         "namespace"_attr = toStringForLogging(_nssOrUUID),
                         "attempts"_attr = _wceCount);
}

void WriteConflictRetryAlgorithm::_assertRetryLimit() const {
    if (MONGO_unlikely(_retryLimit && _wceCount > *_retryLimit)) {
        LOGV2_ERROR(7677402, "Got too many write conflicts, the server may run into problems.");
        fassert(7677401, !getTestCommandsEnabled());
    }
}

void WriteConflictRetryAlgorithm::_handleStorageUnavailable(const Status& status) {
    ++_attemptCount;
    // To make the happy path faster we don't start timing until after the first error
    if (_timer)
        _conflictTime += _timer->elapsed();
    switch (status.code()) {
        case ErrorCodes::WriteConflict:
            _handleWriteConflictException(status);
            break;
        case ErrorCodes::TemporarilyUnavailable:
            ++_tempUnavailableCount;
            handleTemporarilyUnavailableException(_opCtx,
                                                  _recoveryUnit(),
                                                  _tempUnavailableCount,
                                                  _opStr,
                                                  _nssOrUUID,
                                                  status,
                                                  _wceCount);
            break;
        case ErrorCodes::TransactionTooLargeForCache:
            handleTransactionTooLargeForCacheException(
                _opCtx, _recoveryUnit(), _opStr, _nssOrUUID, status, _wceCount);
            ++_wceCount;
            break;
        default:
            // Currently unreachable, but a reasonable fallback if a new error is added
            throw;
    }

    // Either creates the timer or resets it if it already exists, as we only want to measure the
    // time spent on the operation being retried and not the time spent sleeping.
    _timer.emplace();
}


/**
 * Sleeps for (10% of average attempt time) * 1.1^attempt. Experimentally a very gentle
 * exponential slow appears to work well for many operations; under very heavy load it's
 * important that we eventually reach significant backoff but under light load sleeping
 * significantly hurts performance, and "light load" includes scenarios where each write
 * takes several attempts.
 */
void WriteConflictRetryAlgorithm::_handleWriteConflictException(const Status& s) {
    ++_wceCount;
    if (MONGO_unlikely(_dumpStateRetryCount && (_wceCount % _dumpStateRetryCount) == 0)) {
        _opCtx->getServiceContext()->getStorageEngine()->dump();
    }
    recordWriteConflict(_opCtx);
    _recoveryUnit().abandonSnapshot();
    _emitLog(s.reason());

    sleepFor(floatScaleDuration(_backoffFactor / _attemptCount, _conflictTime));
    _backoffFactor *= backoffGrowth;

    _assertRetryLimit();
}

}  // namespace mongo
