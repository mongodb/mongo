// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_change_stream_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

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
                               DocumentSourceChangeStream::kReshardDoneCatchUpOpType,
                               DocumentSourceChangeStream::kNewShardDetectedOpType};
}  // namespace mongo::change_stream
