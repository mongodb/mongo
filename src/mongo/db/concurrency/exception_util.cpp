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


#include "mongo/db/concurrency/exception_util.h"

#include "mongo/db/commands/server_status_metric.h"
#include "mongo/db/concurrency/exception_util_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/logv2/log.h"
#include "mongo/util/duration.h"
#include "mongo/util/log_and_backoff.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {

MONGO_FAIL_POINT_DEFINE(skipWriteConflictRetries);

void logWriteConflictAndBackoff(int attempt, StringData operation, StringData ns) {
    logAndBackoff(4640401,
                  logv2::LogComponent::kWrite,
                  logv2::LogSeverity::Debug(1),
                  static_cast<size_t>(attempt),
                  "Caught WriteConflictException",
                  "operation"_attr = operation,
                  logAttrs(NamespaceString(ns)));
}

namespace {

CounterMetric temporarilyUnavailableErrors{"operation.temporarilyUnavailableErrors"};
CounterMetric temporarilyUnavailableErrorsEscaped{"operation.temporarilyUnavailableErrorsEscaped"};
CounterMetric temporarilyUnavailableErrorsConvertedToWriteConflict{
    "operation.temporarilyUnavailableErrorsConvertedToWriteConflict"};

}  // namespace

void handleTemporarilyUnavailableException(OperationContext* opCtx,
                                           int attempts,
                                           StringData opStr,
                                           StringData ns,
                                           const TemporarilyUnavailableException& e) {
    opCtx->recoveryUnit()->abandonSnapshot();
    temporarilyUnavailableErrors.increment(1);
    if (opCtx->getClient()->isFromUserConnection() &&
        attempts > gTemporarilyUnavailableExceptionMaxRetryAttempts.load()) {
        LOGV2_DEBUG(6083901,
                    1,
                    "Too many TemporarilyUnavailableException's, giving up",
                    "reason"_attr = e.reason(),
                    "attempts"_attr = attempts,
                    "operation"_attr = opStr,
                    logAttrs(NamespaceString(ns)));
        temporarilyUnavailableErrorsEscaped.increment(1);
        throw e;
    }

    // Back off linearly with the retry attempt number.
    auto sleepFor =
        Milliseconds(gTemporarilyUnavailableExceptionRetryBackoffBaseMs.load()) * attempts;
    LOGV2_DEBUG(6083900,
                1,
                "Caught TemporarilyUnavailableException",
                "reason"_attr = e.reason(),
                "attempts"_attr = attempts,
                "operation"_attr = opStr,
                "sleepFor"_attr = sleepFor,
                logAttrs(NamespaceString(ns)));
    opCtx->sleepFor(sleepFor);
}

void handleTemporarilyUnavailableExceptionInTransaction(OperationContext* opCtx,
                                                        StringData opStr,
                                                        StringData ns,
                                                        const TemporarilyUnavailableException& e) {
    // Since WriteConflicts are tagged as TransientTransactionErrors and TemporarilyUnavailable
    // errors are not, we convert the error to a WriteConflict to allow users of multi-document
    // transactions to retry without changing any behavior. Otherwise, we let the error escape as
    // usual.
    temporarilyUnavailableErrorsConvertedToWriteConflict.increment(1);
    throwWriteConflictException(e.reason());
}

}  // namespace mongo
