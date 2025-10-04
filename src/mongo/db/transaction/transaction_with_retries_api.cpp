/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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


#include "mongo/db/operation_context.h"
#include "mongo/db/session/session_catalog.h"
#include "mongo/db/transaction/internal_transaction_metrics.h"
#include "mongo/db/transaction/transaction_api.h"
#include "mongo/logv2/log.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/future_util.h"


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTransaction


namespace mongo {

const Backoff kExponentialBackoff(Seconds(1), Milliseconds::max());


namespace txn_api::details {
std::string errorHandlingStepToString(Transaction::ErrorHandlingStep nextStep) {
    switch (nextStep) {
        case Transaction::ErrorHandlingStep::kDoNotRetry:
            return "do not retry";
        case Transaction::ErrorHandlingStep::kAbortAndDoNotRetry:
            return "abort and do not retry";
        case Transaction::ErrorHandlingStep::kRetryTransaction:
            return "retry transaction";
        case Transaction::ErrorHandlingStep::kRetryCommit:
            return "retry commit";
    }
    MONGO_UNREACHABLE;
}

void logNextStep(Transaction::ErrorHandlingStep nextStep,
                 const BSONObj& txnInfo,
                 int attempts,
                 const StatusWith<CommitResult>& swResult,
                 StringData errorHandler) {
    // DynamicAttributes doesn't allow rvalues, so make some local variables.
    auto nextStepString = errorHandlingStepToString(nextStep);
    std::string redactedError, redactedCommitError, redactedCommitWCError;

    logv2::DynamicAttributes attr;
    attr.add("nextStep", nextStepString);
    attr.add("txnInfo", txnInfo);
    attr.add("attempts", attempts);
    if (!swResult.isOK()) {
        redactedError = redact(swResult.getStatus());
        attr.add("error", redactedError);
    } else {
        redactedCommitError = redact(swResult.getValue().cmdStatus);
        attr.add("commitError", redactedCommitError);
        redactedCommitWCError = redact(swResult.getValue().wcError.toStatus());
        attr.add("commitWCError", redactedCommitWCError);
    }
    attr.add("errorHandler", errorHandler);

    LOGV2(5918600, "Chose internal transaction error handling step", attr);
}

SemiFuture<CommitResult> TransactionWithRetries::run(Callback callback) noexcept {
    InternalTransactionMetrics::get(_internalTxn->getParentServiceContext())->incrementStarted();
    _internalTxn->setCallback(std::move(callback));

    return AsyncTry([this, bodyAttempts = 0]() mutable {
               bodyAttempts++;
               return _runBodyHandleErrors(bodyAttempts).then([this] {
                   return _runCommitWithRetries();
               });
           })
        .until([](StatusOrStatusWith<CommitResult> txnStatus) {
            // Commit retries should be handled within _runCommitWithRetries().
            invariant(txnStatus != ErrorCodes::TransactionAPIMustRetryCommit);

            return txnStatus.isOK() || txnStatus != ErrorCodes::TransactionAPIMustRetryTransaction;
        })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(_executor, _token)
        // Safe to inline because the continuation only holds state.
        .unsafeToInlineFuture()
        .tapAll([anchor = shared_from_this()](auto&&) {})
        .semi();
}

ExecutorFuture<void> TransactionWithRetries::_runBodyHandleErrors(int bodyAttempts) {
    return _internalTxn->runCallback().thenRunOn(_executor).onError(
        [this, bodyAttempts](Status bodyStatus) {
            auto nextStep = _internalTxn->handleError(bodyStatus, bodyAttempts);
            logNextStep(
                nextStep, _internalTxn->reportStateForLog(), bodyAttempts, bodyStatus, "runBody");

            if (nextStep == Transaction::ErrorHandlingStep::kDoNotRetry) {
                iassert(bodyStatus);
            } else if (nextStep == Transaction::ErrorHandlingStep::kAbortAndDoNotRetry) {
                _internalTxn->primeForCleanup();
                iassert(bodyStatus);
            } else if (nextStep == Transaction::ErrorHandlingStep::kRetryTransaction) {
                return _bestEffortAbort().then([this, bodyStatus](AbortResult result) {
                    InternalTransactionMetrics::get(_internalTxn->getParentServiceContext())
                        ->incrementRetriedTransactions();
                    _internalTxn->primeForTransactionRetry();
                    iassert(Status(ErrorCodes::TransactionAPIMustRetryTransaction,
                                   str::stream() << "Must retry body loop on internal body error: "
                                                 << bodyStatus));
                });
            }
            MONGO_UNREACHABLE;
        });
}

ExecutorFuture<CommitResult> TransactionWithRetries::_runCommitHandleErrors(int commitAttempts) {
    return _internalTxn->commit().thenRunOn(_executor).onCompletion(
        [this, commitAttempts](StatusWith<CommitResult> swCommitResult) {
            if (swCommitResult.isOK() && swCommitResult.getValue().getEffectiveStatus().isOK()) {
                InternalTransactionMetrics::get(_internalTxn->getParentServiceContext())
                    ->incrementSucceeded();
                // Commit succeeded so return to the caller.
                return ExecutorFuture<CommitResult>(_executor, swCommitResult);
            }

            auto nextStep = _internalTxn->handleError(swCommitResult, commitAttempts);
            logNextStep(nextStep,
                        _internalTxn->reportStateForLog(),
                        commitAttempts,
                        swCommitResult,
                        "runCommit");

            if (nextStep == Transaction::ErrorHandlingStep::kDoNotRetry) {
                return ExecutorFuture<CommitResult>(_executor, swCommitResult);
            } else if (nextStep == Transaction::ErrorHandlingStep::kAbortAndDoNotRetry) {
                MONGO_UNREACHABLE;
            } else if (nextStep == Transaction::ErrorHandlingStep::kRetryTransaction) {
                InternalTransactionMetrics::get(_internalTxn->getParentServiceContext())
                    ->incrementRetriedTransactions();
                _internalTxn->primeForTransactionRetry();
                iassert(Status(ErrorCodes::TransactionAPIMustRetryTransaction,
                               str::stream() << "Must retry body loop on commit error: "
                                             << swCommitResult.getStatus()));
            } else if (nextStep == Transaction::ErrorHandlingStep::kRetryCommit) {
                InternalTransactionMetrics::get(_internalTxn->getParentServiceContext())
                    ->incrementRetriedCommits();
                _internalTxn->primeForCommitRetry();
                iassert(Status(ErrorCodes::TransactionAPIMustRetryCommit,
                               str::stream() << "Must retry commit loop on internal commit error: "
                                             << swCommitResult.getStatus()));
            }
            MONGO_UNREACHABLE;
        });
}

ExecutorFuture<CommitResult> TransactionWithRetries::_runCommitWithRetries() {
    return AsyncTry([this, commitAttempts = 0]() mutable {
               commitAttempts++;
               return _runCommitHandleErrors(commitAttempts);
           })
        .until([](StatusWith<CommitResult> swResult) {
            return swResult.isOK() || swResult != ErrorCodes::TransactionAPIMustRetryCommit;
        })
        .withBackoffBetweenIterations(kExponentialBackoff)
        .on(_executor, _token);
}

ExecutorFuture<AbortResult> TransactionWithRetries::_bestEffortAbort() {
    return _internalTxn->abort().thenRunOn(_executor).then([this](AbortResult result) {
        if (!result.cmdStatus.isOK() || !result.wcError.toStatus().isOK()) {
            LOGV2(5875900,
                  "Unable to abort internal transaction",
                  "commandStatus"_attr = redact(result.cmdStatus),
                  "writeConcernStatus"_attr = redact(result.wcError.toStatus()),
                  "txnInfo"_attr = _internalTxn->reportStateForLog());
        }
        return result;
    });
}

bool TransactionWithRetries::needsCleanup() {
    return _internalTxn->needsCleanup();
}

SemiFuture<AbortResult> TransactionWithRetries::cleanUp() {
    tassert(7567600, "Unnecessarily cleaning up transaction", _internalTxn->needsCleanup());

    return _bestEffortAbort()
        // Safe to inline because the continuation only holds state.
        .unsafeToInlineFuture()
        .tapAll([anchor = shared_from_this()](auto&&) {})
        .semi();
}


}  // namespace txn_api::details

}  // namespace mongo
