/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_change_stream_add_post_image.h"
#include "mongo/db/pipeline/document_source_change_stream_add_pre_image.h"
#include "mongo/db/pipeline/document_source_change_stream_ensure_resume_token_present.h"
#include "mongo/db/pipeline/document_source_change_stream_handle_topology_change.h"
#include "mongo/db/pipeline/document_source_change_stream_handle_topology_change_v2.h"
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

// These will become redundant in a follow-up PR under SERVER-110104.
REGISTER_RULES(DocumentSourceChangeStreamAddPostImage,
               OPTIMIZE_AT_RULE(DocumentSourceChangeStreamAddPostImage));
REGISTER_RULES(DocumentSourceChangeStreamAddPreImage,
               OPTIMIZE_AT_RULE(DocumentSourceChangeStreamAddPreImage));
REGISTER_RULES(DocumentSourceChangeStreamEnsureResumeTokenPresent,
               OPTIMIZE_AT_RULE(DocumentSourceChangeStreamEnsureResumeTokenPresent));
REGISTER_RULES(DocumentSourceChangeStreamHandleTopologyChangeV2,
               OPTIMIZE_AT_RULE(DocumentSourceChangeStreamHandleTopologyChangeV2));
REGISTER_RULES(DocumentSourceChangeStreamHandleTopologyChange,
               OPTIMIZE_AT_RULE(DocumentSourceChangeStreamHandleTopologyChange));

}  // namespace mongo::rule_based_rewrites::pipeline
