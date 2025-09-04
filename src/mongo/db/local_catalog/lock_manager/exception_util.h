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
#include "mongo/base/string_data.h"
#include "mongo/db/client.h"
#include "mongo/db/local_catalog/lock_manager/exception_util_gen.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/logv2/log_severity_suppressor.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"

#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl

namespace mongo {

extern FailPoint skipWriteConflictRetries;

/**
 * Will record an occurrence of write conflict on this operation's metrics.
 */
void recordWriteConflict(OperationContext* opCtx, int64_t n = 1);

/**
 * Will record an occurrence of temporarility unavailable error on this operation's metrics.
 */
void recordTemporarilyUnavailableErrors(OperationContext* opCtx, int64_t n = 1);

/**
 * Will log a message if sensible and will do an increasing backoff to make sure we don't hammer
 * the same doc over and over. This function does not increases the Write Conflict metric.
 * @param attempt - what attempt is this, 1 based
 * @param operation - e.g. "update"
 */
void logWriteConflictAndBackoff(size_t attempt,
                                StringData operation,
                                StringData reason,
                                const NamespaceStringOrUUID& nssOrUUID);

/**
 * Similar to the above function, it will log a message if sensible and will do an increasing
 * backoff to make sure we don't hammer the same doc over and over. This function will increase the
 * Write Conflict metrics.
 * @param attempt - what attempt is this, 1 based
 * @param operation - e.g. "update"
 */
void logAndRecordWriteConflictAndBackoff(OperationContext* opCtx,
                                         size_t attempt,
                                         StringData operation,
                                         StringData reason,
                                         const NamespaceStringOrUUID& nssOrUUID);

/**
 * Retries the operation for a fixed number of attempts with linear backoff.
 * For internal system operations, converts the temporarily unavailable error into a write
 * conflict and handles it, because unlike user operations, the error cannot eventually escape to
 * the client.
 *
 * TODO (SERVER-105773): Remove the overload without RecoveryUnit.
 */
void handleTemporarilyUnavailableException(OperationContext* opCtx,
                                           size_t tempUnavailAttempts,
                                           StringData opStr,
                                           const NamespaceStringOrUUID& nssOrUUID,
                                           const Status& e,
                                           size_t& writeConflictAttempts);
void handleTemporarilyUnavailableException(OperationContext* opCtx,
                                           RecoveryUnit& ru,
                                           size_t tempUnavailAttempts,
                                           StringData opStr,
                                           const NamespaceStringOrUUID& nssOrUUID,
                                           const Status& e,
                                           size_t& writeConflictAttempts);

/**
 * Convert `e` into a `WriteConflictException` and throw it.
 */
void convertToWCEAndRethrow(OperationContext* opCtx,
                            StringData opStr,
                            const ExceptionFor<ErrorCodes::TemporarilyUnavailable>& e);

/** Stateful object for executing the `writeConflictRetry` function below. */
class WriteConflictRetryAlgorithm {
public:
    static constexpr double backoffInitial = .1;
    static constexpr double backoffGrowth = 1.1;

    WriteConflictRetryAlgorithm(OperationContext* opCtx,
                                RecoveryUnit& ru,
                                StringData opStr,
                                const NamespaceStringOrUUID& nssOrUUID,
                                boost::optional<size_t> retryLimit,
                                int dumpStateRetryCount)
        : _opCtx{opCtx},
          _ru(ru),
          _opStr{opStr},
          _nssOrUUID{nssOrUUID},
          _retryLimit{retryLimit},
          _dumpStateRetryCount(
              dumpStateRetryCount
                  ? std::max(dumpStateRetryCount,
                             gMinimalWriteConflictRetryCountForStateDump.loadRelaxed())
                  : 0) {
        invariant(_opCtx);
        invariant(shard_role_details::getLocker(_opCtx));
    }
    WriteConflictRetryAlgorithm(OperationContext* opCtx,
                                std::function<RecoveryUnit&()> ru,
                                StringData opStr,
                                const NamespaceStringOrUUID& nssOrUUID,
                                boost::optional<size_t> retryLimit,
                                int dumpStateRetryCount)
        : _opCtx{opCtx},
          _ru(std::move(ru)),
          _opStr{opStr},
          _nssOrUUID{nssOrUUID},
          _retryLimit{retryLimit},
          _dumpStateRetryCount(
              dumpStateRetryCount
                  ? std::max(dumpStateRetryCount,
                             gMinimalWriteConflictRetryCountForStateDump.loadRelaxed())
                  : 0) {
        invariant(_opCtx);
        invariant(shard_role_details::getLocker(_opCtx));
    }

    /** Returns whatever `f` returns. */
    decltype(auto) operator()(auto&& f) {
        // Always run without retries in a WuoW because the entire WuoW needs to be retried.
        if (shard_role_details::getLocker(_opCtx)->inAWriteUnitOfWork())
            return _runWithoutRetries(f);

        // This failpoint disables exception handling for write conflicts. Only
        // allow this exception to escape user operations. Do not allow exceptions
        // to escape internal threads, which may rely on this exception handler to
        // avoid crashing.
        // We avoid "entering" the FailPoint until we really need to.
        if (auto sfp = skipWriteConflictRetries.scopedIf(
                [&](auto&&) { return _opCtx->getClient()->isFromUserConnection(); });
            MONGO_unlikely(sfp.isActive()))
            return _runWithoutRetries(f);

        while (true) {
            try {
                return f();
            } catch (const StorageUnavailableException& e) {
                _handleStorageUnavailable(e.toStatus());
            }
        }
    }

private:
    decltype(auto) _runWithoutRetries(auto&& f) {
        try {
            return f();
        } catch (const ExceptionFor<ErrorCodes::TemporarilyUnavailable>& e) {
            if (_opCtx->inMultiDocumentTransaction()) {
                convertToWCEAndRethrow(_opCtx, _opStr, e);
            }
            throw;
        } catch (const ExceptionFor<ErrorCodes::WriteConflict>&) {
            recordWriteConflict(_opCtx);
            throw;
        }
    }
    void _emitLog(StringData reason);
    void _assertRetryLimit() const;
    void _handleStorageUnavailable(const Status& e);
    void _handleWriteConflictException(const Status& e);

    RecoveryUnit& _recoveryUnit() const {
        return visit(
            OverloadedVisitor{
                [](std::reference_wrapper<RecoveryUnit> ru) -> RecoveryUnit& { return ru; },
                [](const std::function<RecoveryUnit&()>& ru) -> RecoveryUnit& {
                    return ru();
                }},
            _ru);
    }

    OperationContext* const _opCtx;
    std::variant<std::reference_wrapper<RecoveryUnit>, std::function<RecoveryUnit&()>> _ru;
    const StringData _opStr;
    const NamespaceStringOrUUID& _nssOrUUID;
    const boost::optional<size_t> _retryLimit;
    const int _dumpStateRetryCount = 0;
    size_t _attemptCount = 0;
    size_t _wceCount = 0;
    size_t _tempUnavailableCount = 0;
    boost::optional<Timer> _timer;
    Microseconds _conflictTime{0};
    double _backoffFactor = backoffInitial;
    logv2::SeveritySuppressor _logSeverity{
        Seconds(30), logv2::LogSeverity::Info(), logv2::LogSeverity::Debug(1)};
};

/**
 * Runs the argument function f as many times as needed for f to complete or throw an exception
 * other than WriteConflictException or TemporarilyUnavailableException. For each time f throws
 * one of these exceptions, logs the error, waits a spell, cleans up, and then tries f again.
 * Imposes no upper limit on the number of times to re-try f after a WriteConflictException, so any
 * required timeout behavior must be enforced within f. When retrying a
 * TemporarilyUnavailableException, f is called a finite number of times before we eventually let
 * the error escape.
 *
 * If we are already in a WriteUnitOfWork, we assume that we are being called within a
 * WriteConflictException retry loop up the call stack. Hence, this retry loop is reduced to an
 * invocation of the argument function f without any exception handling and retry logic.
 *
 * TODO (SERVER-105773): Remove the overload without RecoveryUnit.
 */
template <typename F>
auto writeConflictRetry(
    OperationContext* opCtx,
    RecoveryUnit& ru,
    StringData opStr,
    const NamespaceStringOrUUID& nssOrUUID,
    F&& f,
    boost::optional<size_t> retryLimit = boost::none,
    /* Dump the WT state on every N times when you hit a WCE, where N == dumpStateRetryCount. */
    int dumpStateRetryCount = 0) {
    return WriteConflictRetryAlgorithm{
        opCtx, ru, opStr, nssOrUUID, retryLimit, dumpStateRetryCount}(std::forward<F>(f));
}
template <typename F>
auto writeConflictRetry(
    OperationContext* opCtx,
    StringData opStr,
    const NamespaceStringOrUUID& nssOrUUID,
    F&& f,
    boost::optional<size_t> retryLimit = boost::none,
    /* Dump the WT state on every N times when you hit a WCE, where N == dumpStateRetryCount. */
    int dumpStateRetryCount = 0) {
    return WriteConflictRetryAlgorithm{
        opCtx,
        [opCtx]() -> RecoveryUnit& { return *shard_role_details::getRecoveryUnit(opCtx); },
        opStr,
        nssOrUUID,
        retryLimit,
        dumpStateRetryCount}(std::forward<F>(f));
}

}  // namespace mongo

#undef MONGO_LOGV2_DEFAULT_COMPONENT
