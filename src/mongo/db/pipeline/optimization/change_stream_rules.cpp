// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_change_stream_oplog_match.h"
#include "mongo/db/pipeline/document_source_change_stream_split_large_event.h"
#include "mongo/db/pipeline/document_source_change_stream_unwind_transaction.h"
#include "mongo/db/pipeline/optimization/rule_based_rewriter.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::rule_based_rewrites::pipeline {

REGISTER_RULES(DocumentSourceChangeStreamUnwindTransaction,
               OPTIMIZE_AT_RULE(DocumentSourceChangeStreamUnwindTransaction));
REGISTER_RULES(DocumentSourceChangeStreamOplogMatch,
               OPTIMIZE_AT_RULE(DocumentSourceChangeStreamOplogMatch));
REGISTER_RULES(DocumentSourceChangeStreamSplitLargeEvent,
               OPTIMIZE_AT_RULE(DocumentSourceChangeStreamSplitLargeEvent));

}  // namespace mongo::rule_based_rewrites::pipeline
