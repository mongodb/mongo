/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/error_labels.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/util/exit.h"

namespace mongo {

namespace {

MONGO_FAIL_POINT_DEFINE(errorLabelBuilderMockShutdown);

const StringMap<int> commitOrAbortCommands = {{"abortTransaction", 1},
                                              {"clusterAbortTransaction", 1},
                                              {"clusterCommitTransaction", 1},
                                              {"commitTransaction", 1},
                                              {"coordinateCommitTransaction", 1}};

}  // namespace

bool ErrorLabelBuilder::isTransientTransactionError() const {
    // Note that we only apply the TransientTransactionError label if the "autocommit" field is
    // present in the session options. When present, "autocommit" will always be false, so we
    // don't check its value. There is no point in returning TransientTransactionError label if
    // we have already tried to abort it. An error code for which isTransientTransactionError()
    // is true indicates a transaction failure with no persistent side effects.
    return _code && _sessionOptions.getTxnNumber() && _sessionOptions.getAutocommit() &&
        mongo::isTransientTransactionError(_code.get(), _wcCode != boost::none, _isCommitOrAbort());
}

bool ErrorLabelBuilder::isRetryableWriteError() const {
    // Do not return RetryableWriteError labels to internal clients (e.g. mongos).
    if (_isInternalClient) {
        return false;
    }

    auto isRetryableWrite = [&]() {
        return _sessionOptions.getTxnNumber() && !_sessionOptions.getAutocommit();
    };

    auto isTransactionCommitOrAbort = [&]() {
        return _sessionOptions.getTxnNumber() && _sessionOptions.getAutocommit() &&
            _isCommitOrAbort();
    };

    // Return with RetryableWriteError label on retryable error codes for retryable writes or
    // transactions commit/abort.
    if (isRetryableWrite() || isTransactionCommitOrAbort()) {
        bool isShutDownCode = _code &&
            (ErrorCodes::isShutdownError(_code.get()) ||
             _code.get() == ErrorCodes::CallbackCanceled);
        if (isShutDownCode &&
            (globalInShutdownDeprecated() ||
             MONGO_unlikely(errorLabelBuilderMockShutdown.shouldFail()))) {
            return true;
        }

        // mongos should not attach RetryableWriteError label to retryable errors thrown by the
        // config server or targeted shards.
        return !_isMongos &&
            ((_code && ErrorCodes::isRetriableError(_code.get())) ||
             (_wcCode && ErrorCodes::isRetriableError(_wcCode.get())));
    }
    return false;
}

bool ErrorLabelBuilder::isNonResumableChangeStreamError() const {
    return _code && ErrorCodes::isNonResumableChangeStreamError(_code.get());
}

bool ErrorLabelBuilder::isResumableChangeStreamError() const {
    // Determine whether this operation is a candidate for the ResumableChangeStreamError label.
    const bool mayNeedResumableChangeStreamErrorLabel =
        (_commandName == "aggregate" || _commandName == "getMore") && _code && !_wcCode &&
        (ErrorCodes::isRetriableError(*_code) || ErrorCodes::isNetworkError(*_code) ||
         ErrorCodes::isNeedRetargettingError(*_code) || _code == ErrorCodes::RetryChangeStream ||
         _code == ErrorCodes::FailedToSatisfyReadPreference);

    // If the command or exception is not relevant, bail out early.
    if (!mayNeedResumableChangeStreamErrorLabel) {
        return false;
    }

    // We should always have a valid opCtx at this point.
    invariant(_opCtx);

    // Get the full command object from CurOp. If this is a getMore, get the original command.
    const auto cmdObj = (_commandName == "aggregate" ? CurOp::get(_opCtx)->opDescription()
                                                     : CurOp::get(_opCtx)->originatingCommand());

    // Get the namespace string from CurOp. We will need it to build the LiteParsedPipeline.
    const auto nss = NamespaceString{CurOp::get(_opCtx)->getNS()};

    bool apiStrict = APIParameters::get(_opCtx).getAPIStrict().value_or(false);
    // Do enough parsing to confirm that this is a well-formed pipeline with a $changeStream.
    const auto swLitePipe = [this, &nss, &cmdObj, apiStrict]() -> StatusWith<LiteParsedPipeline> {
        try {
            auto aggRequest = aggregation_request_helper::parseFromBSON(
                _opCtx, nss, cmdObj, boost::none, apiStrict);
            return LiteParsedPipeline(aggRequest);
        } catch (const DBException& ex) {
            return ex.toStatus();
        }
    }();

    // If the pipeline parsed successfully and is a $changeStream, add the label.
    return swLitePipe.isOK() && swLitePipe.getValue().hasChangeStream();
}

bool ErrorLabelBuilder::isErrorWithNoWritesPerformed() const {
    if (!_code && !_wcCode) {
        return false;
    }
    if (_lastOpBeforeRun.isNull() || _lastOpAfterRun.isNull()) {
        // Last OpTimes are unknown or not usable for determining whether or not a write was
        // attempted.
        return false;
    }
    return _lastOpBeforeRun == _lastOpAfterRun;
}

void ErrorLabelBuilder::build(BSONArrayBuilder& labels) const {
    // PLEASE CONSULT DRIVERS BEFORE ADDING NEW ERROR LABELS.
    bool hasTransientTransactionOrRetryableWriteError = false;
    if (isTransientTransactionError()) {
        labels << ErrorLabel::kTransientTransaction;
        hasTransientTransactionOrRetryableWriteError = true;
    } else {
        if (isRetryableWriteError()) {
            // In the rare case where RetryableWriteError and TransientTransactionError are not
            // mutually exclusive, only append the TransientTransactionError label so users know to
            // retry the entire transaction.
            labels << ErrorLabel::kRetryableWrite;
            hasTransientTransactionOrRetryableWriteError = true;
            if (isErrorWithNoWritesPerformed()) {
                // The NoWritesPerformed error label is only relevant for retryable writes so that
                // drivers can determine what error to return when faced with multiple errors (see
                // SERVER-66479 and DRIVERS-2327).
                labels << ErrorLabel::kNoWritesPerformed;
            }
        }
    }

    // Change streams cannot run in a transaction, and cannot be a retryable write. Since these
    // labels are only added in the event that we are executing the associated operation, we do
    // not add a ResumableChangeStreamError label if either of them is set.
    if (!hasTransientTransactionOrRetryableWriteError && isResumableChangeStreamError()) {
        labels << ErrorLabel::kResumableChangeStream;
    } else if (isNonResumableChangeStreamError()) {
        labels << ErrorLabel::kNonResumableChangeStream;
    }
}

bool ErrorLabelBuilder::_isCommitOrAbort() const {
    return commitOrAbortCommands.find(_commandName) != commitOrAbortCommands.cend();
}

BSONObj getErrorLabels(OperationContext* opCtx,
                       const OperationSessionInfoFromClient& sessionOptions,
                       const std::string& commandName,
                       boost::optional<ErrorCodes::Error> code,
                       boost::optional<ErrorCodes::Error> wcCode,
                       bool isInternalClient,
                       bool isMongos,
                       const repl::OpTime& lastOpBeforeRun,
                       const repl::OpTime& lastOpAfterRun) {
    if (MONGO_unlikely(errorLabelsOverride(opCtx))) {
        // This command was failed by a failCommand failpoint. Thus, we return the errorLabels
        // specified in the failpoint to supress any other error labels that would otherwise be
        // returned by the ErrorLabelBuilder.
        if (errorLabelsOverride(opCtx).get().isEmpty()) {
            return BSONObj();
        } else {
            return BSON(kErrorLabelsFieldName << errorLabelsOverride(opCtx).get());
        }
    }

    BSONArrayBuilder labelArray;
    ErrorLabelBuilder labelBuilder(opCtx,
                                   sessionOptions,
                                   commandName,
                                   code,
                                   wcCode,
                                   isInternalClient,
                                   isMongos,
                                   lastOpBeforeRun,
                                   lastOpAfterRun);
    labelBuilder.build(labelArray);

    return (labelArray.arrSize() > 0) ? BSON(kErrorLabelsFieldName << labelArray.arr()) : BSONObj();
}

bool isTransientTransactionError(ErrorCodes::Error code,
                                 bool hasWriteConcernError,
                                 bool isCommitOrAbort) {
    if (code == ErrorCodes::InternalTransactionNotSupported) {
        // InternalTransactionNotSupported is a retryable write error. This allows a retryable
        // WouldChangeOwningShard update or findAndModify statement that fails to execute using an
        // internal transaction during downgrade to be retried by the drivers; the retry would use
        // the legacy way of handling WouldChangeOwningShard errors which does not require an
        // internal transaction. Don't label InternalTransactionNotSupported as a transient
        // transaction error since otherwise the transaction API would retry the internal
        // transaction until it exhausts the maximum number of retries before returning an error to
        // the drivers.
        return false;
    }

    bool isTransient;
    switch (code) {
        case ErrorCodes::WriteConflict:
        case ErrorCodes::LockTimeout:
        case ErrorCodes::PreparedTransactionInProgress:
        case ErrorCodes::ShardCannotRefreshDueToLocksHeld:
        case ErrorCodes::ShardInvalidatedForTargeting:
        case ErrorCodes::StaleDbVersion:
        case ErrorCodes::TenantMigrationAborted:
        case ErrorCodes::TenantMigrationCommitted:
        case ErrorCodes::TxnRetryCounterNotSupported:
            return true;
        default:
            isTransient = false;
            break;
    }

    isTransient |= ErrorCodes::isSnapshotError(code) || ErrorCodes::isNeedRetargettingError(code);

    if (isCommitOrAbort) {
        // On NoSuchTransaction it's safe to retry the whole transaction only if the data cannot be
        // rolled back.
        isTransient |= code == ErrorCodes::NoSuchTransaction && !hasWriteConcernError;
    } else {
        isTransient |= ErrorCodes::isRetriableError(code) || code == ErrorCodes::NoSuchTransaction;
    }

    return isTransient;
}

}  // namespace mongo
