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
