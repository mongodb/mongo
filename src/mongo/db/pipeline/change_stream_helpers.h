// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_codes.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/change_stream_error_source.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <exception>
#include <string>
#include <string_view>

#include <boost/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

// Builds an operationType:"error" change stream event from the exception 'ex' being handled, using
// the failing change event in flight ('sourceEvent', the recorded identity of the error; its
// sort-key metadata provides the resume token, which user stages cannot modify). Fails closed with
// ChangeStreamFatalError if '_id' was modified, and rethrows 'ex' if 'sourceEvent' has no resume
// token. 'shardId', when set, is recorded under error.shardId.
Document makeChangeStreamErrorEvent(const DBException& ex,
                                    ChangeStreamErrorSource source,
                                    const Document& sourceEvent,
                                    boost::optional<ShardId> shardId = boost::none);

}  // namespace mongo

namespace mongo::change_stream {

/**
 * Determines how the change stream synthesizes 'endOfTransaction' events for unprepared
 * (single-shard or replica-set) transactions. It is resolved once, when the change stream stage
 * that needs it is created, so that all of its transaction iterators observe the same behavior
 * even if the feature flags or the 'allowEndOfTransaction' server parameter change while the stream
 * is open. The 'featureFlagEndOfTransactionChangeEventSynthesis' behavior takes precedence over the
 * legacy behavior.
 */
enum class EndOfTransactionBehavior {
    // No endOfTransaction events are synthesized: neither the legacy path nor the
    // 'featureFlagEndOfTransactionChangeEventSynthesis' path is enabled.
    kNone,
    // The legacy path is used when 'featureFlagEndOfTransactionChangeEvent' is enabled and the
    // stream requests expanded events, and the 'featureFlagEndOfTransactionChangeEventSynthesis'
    // path is not enabled. The event is synthesized with the index after the last operation.
    // TODO SERVER-138462 Remove feature flag for legacy endOfTransaction behavior.
    kLegacy,
    // Used when 'featureFlagEndOfTransactionChangeEventSynthesis' is enabled, the
    // 'allowEndOfTransaction' server parameter is enabled, and the stream opts in via
    // 'showEndOfTransaction'. It takes precedence over the legacy path. The event is synthesized
    // with the reserved 'kEndOfTransactionTxnOpIndex'.
    kNew,
};

/**
 * Resolves the endOfTransaction synthesis behavior from the feature flags, the
 * 'allowEndOfTransaction' server parameter and the change stream opt-in.
 */
EndOfTransactionBehavior determineEndOfTransactionBehavior(
    const boost::intrusive_ptr<ExpressionContext>& expCtx);

/**
 * Returns whether the user requested endOfTransaction events, i.e. whether the
 * 'showEndOfTransaction' option was set to a value other than 'off'.
 */
bool showEndOfTransactionRequested(const DocumentSourceChangeStreamSpec& spec);

/**
 * Returns whether the user requires endOfTransaction events, i.e. whether the
 * 'showEndOfTransaction' option was set to 'required'. Such a stream must fail when it reads a
 * transaction at its watched scope that carries no endOfTransaction metadata.
 */
bool isEndOfTransactionRequired(const DocumentSourceChangeStreamSpec& spec);

/**
 * Tests if we are currently running on a router or in a non-sharded replica set context.
 */
bool isRouterOrNonShardedReplicaSet(const boost::intrusive_ptr<ExpressionContext>& expCtx);

/**
 * Extracts the resume token from the given spec. If a 'startAtOperationTime' is specified,
 * returns the equivalent high-watermark token. This method should only ever be called on a spec
 * where one of 'resumeAfter', 'startAfter', or 'startAtOperationTime' is populated.
 */
ResumeTokenData resolveResumeTokenFromSpec(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                           const DocumentSourceChangeStreamSpec& spec);

/**
 * Returns true if 'collectionUUID' is a user-visible field of the change events produced for the
 * given change stream spec.
 */
bool shouldEmitCollectionUUIDForChangeEvent(const DocumentSourceChangeStreamSpec& spec);

/**
 * Returns true if this change stream is opted into 'showErrorsAsEvents'. Kept out of the generic
 * ExpressionContext so that class stays change-stream-agnostic.
 */
inline bool changeStreamShowsErrorsAsEvents(const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    const auto& spec = expCtx->getChangeStreamSpec();
    return spec && spec->getShowErrorsAsEvents().value_or(false);
}

/**
 * Records 'doc' on the ExpressionContext as the change stream event in flight above the transform
 * stage, so that a recoverable failure in a stage above it can later be reported against this
 * event. Error events ('operationType: "error"') are not recorded. A result without a resume token
 * (a single-element object sort key) clears the recorded event instead, so that a failure on it is
 * never attributed to an earlier event.
 *
 * The caller must only call this for streams opted in to 'showErrorsAsEvents'.
 */
void recordActiveEventIdentity(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                               const Document& doc);

// Builds an operationType:"error" event for a recoverable failure, using the identity and error
// source recorded on 'expCtx'. Returns boost::none when the stream is not opted in, no identity is
// recorded, or the error is not convertible (the caller rethrows). Pure: it does not mutate
// 'expCtx'; the caller owns clearing the recorded identity/source slots.
boost::optional<Document> tryBuildErrorEvent(const DBException& ex,
                                             const boost::intrusive_ptr<ExpressionContext>& expCtx);

// Transient infrastructure failures (stale routing, snapshot, write conflict, lock, shard
// availability, oplog position, cancellation) always propagate, also with showErrorsAsEvents:true:
// converting one would skip an event that a resume would deliver.
inline bool isTransientChangeStreamError(ErrorCodes::Error code) {
    if (ErrorCodes::isA<ErrorCategory::SnapshotError>(code) ||
        ErrorCodes::isA<ErrorCategory::CancellationError>(code) ||
        ErrorCodes::isA<ErrorCategory::NotPrimaryError>(code) ||
        ErrorCodes::isA<ErrorCategory::ShutdownError>(code) ||
        ErrorCodes::isA<ErrorCategory::StaleShardVersionError>(code))
        return true;
    switch (code) {
        case ErrorCodes::StaleDbVersion:
        case ErrorCodes::ShardNotFound:
        case ErrorCodes::MovePrimaryInProgress:
        case ErrorCodes::ShardCannotRefreshDueToLocksHeld:
        case ErrorCodes::FailedToSatisfyReadPreference:
        case ErrorCodes::WriteConflict:
        case ErrorCodes::TemporarilyUnavailable:
        case ErrorCodes::LockBusy:
        case ErrorCodes::CappedPositionLost:
        case ErrorCodes::OplogQueryMinTsMissing:
            return true;
        default:
            return false;
    }
}

// Returns true if a document-scoped failure may be converted into an operationType:"error" event
// when showErrorsAsEvents:true. False for infrastructure errors that must propagate regardless of
// the option, and for server-internal errors that must never reach the client as an event.
inline bool isConvertibleChangeStreamError(ErrorCodes::Error code) {
    if (ErrorCodes::isA<ErrorCategory::Interruption>(code))
        return false;
    if (ErrorCodes::isA<ErrorCategory::RetriableError>(code))
        return false;
    if (ErrorCodes::isA<ErrorCategory::NetworkError>(code))
        return false;
    if (ErrorCodes::isA<ErrorCategory::NeedRetargettingError>(code))
        return false;
    if (ErrorCodes::isA<ErrorCategory::CursorInvalidatedError>(code))
        return false;
    if (ErrorCodes::isA<ErrorCategory::ExceededTimeLimitError>(code))
        return false;
    if (isTransientChangeStreamError(code))
        return false;
    // InternalOnly codes must never be returned to the client, so they must never be embedded in
    // an error event. SystemOverloadedError is transient infrastructure pressure, not a
    // document-scoped failure. InternalError is a server bug.
    if (ErrorCodes::isA<ErrorCategory::InternalOnly>(code))
        return false;
    if (ErrorCodes::isA<ErrorCategory::SystemOverloadedError>(code))
        return false;
    if (code == ErrorCodes::InternalError)
        return false;
    // Within NonResumableChangeStreamError, only these two are truly unrecoverable:
    if (code == ErrorCodes::ChangeStreamFatalError)
        return false;
    if (code == ErrorCodes::ShardRemovedError)
        return false;
    // Change stream control flow; never thrown by user stages.
    if (code == ErrorCodes::RetryChangeStream || code == ErrorCodes::ChangeStreamInvalidated ||
        code == ErrorCodes::ChangeStreamStartAfterInvalidate)
        return false;
    return true;
}

/**
 * Creates endOfTransaction no-op oplog entry
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]]
repl::MutableOplogEntry createEndOfTransactionOplogEntry(
    const LogicalSessionId& lsid,
    const TxnNumber& txnNumber,
    const std::vector<NamespaceString>& affectedNamespaces,
    Timestamp timestamp,
    Date_t wallClock);

/**
 * Represents the change stream operation types that are NOT guarded behind the 'showExpandedEvents'
 * flag.
 */
static const std::set<std::string_view> kClassicOperationTypes =
    std::set<std::string_view>{DocumentSourceChangeStream::kUpdateOpType,
                               DocumentSourceChangeStream::kDeleteOpType,
                               DocumentSourceChangeStream::kReplaceOpType,
                               DocumentSourceChangeStream::kInsertOpType,
                               DocumentSourceChangeStream::kDropCollectionOpType,
                               DocumentSourceChangeStream::kRenameCollectionOpType,
                               DocumentSourceChangeStream::kDropDatabaseOpType,
                               DocumentSourceChangeStream::kInvalidateOpType,
                               DocumentSourceChangeStream::kReshardBeginOpType,
                               DocumentSourceChangeStream::kReshardBlockingWritesOpType,
                               DocumentSourceChangeStream::kReshardDoneCatchUpOpType};
}  // namespace mongo::change_stream
