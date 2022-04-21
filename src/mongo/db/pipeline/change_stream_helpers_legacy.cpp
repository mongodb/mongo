/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/db/pipeline/change_stream_helpers_legacy.h"

#include "mongo/db/pipeline/change_stream_filter_helpers.h"
#include "mongo/db/pipeline/document_source_change_stream_add_post_image.h"
#include "mongo/db/pipeline/document_source_change_stream_add_pre_image.h"
#include "mongo/db/pipeline/document_source_change_stream_check_invalidate.h"
#include "mongo/db/pipeline/document_source_change_stream_check_resumability.h"
#include "mongo/db/pipeline/document_source_change_stream_check_topology_change.h"
#include "mongo/db/pipeline/document_source_change_stream_ensure_resume_token_present.h"
#include "mongo/db/pipeline/document_source_change_stream_handle_topology_change.h"
#include "mongo/db/pipeline/document_source_change_stream_oplog_match.h"
#include "mongo/db/pipeline/document_source_change_stream_transform.h"
#include "mongo/db/pipeline/document_source_change_stream_unwind_transaction.h"
#include "mongo/db/pipeline/expression.h"

namespace mongo {
namespace change_stream_legacy {

std::list<boost::intrusive_ptr<DocumentSource>> buildPipeline(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, DocumentSourceChangeStreamSpec spec) {
    // The only case where we expect to build a legacy pipeline is if we are a shard which has
    // received a $changeStream request from an older mongoS.
    tassert(5565900,
            "Unexpected {needsMerge:false} request for a legacy change stream pipeline",
            expCtx->needsMerge);

    std::list<boost::intrusive_ptr<DocumentSource>> stages;

    const auto userRequestedResumePoint =
        spec.getResumeAfter() || spec.getStartAfter() || spec.getStartAtOperationTime();

    if (!userRequestedResumePoint) {
        // Make sure we update the 'resumeAfter' in the 'spec' so that we serialize the
        // correct resume token when sending it to the shards.
        auto clusterTime = DocumentSourceChangeStream::getStartTimeForNewStream(expCtx);
        spec.setResumeAfter(
            ResumeToken::makeHighWaterMarkToken(clusterTime, expCtx->changeStreamTokenVersion));
    }

    // Unfold the $changeStream into its constituent stages and add them to the pipeline.
    stages.push_back(DocumentSourceChangeStreamOplogMatch::create(expCtx, spec));
    stages.push_back(DocumentSourceChangeStreamUnwindTransaction::create(expCtx));
    stages.push_back(DocumentSourceChangeStreamTransform::create(expCtx, spec));

    tassert(5467606,
            "'DocumentSourceChangeStreamTransform' stage should populate "
            "'initialPostBatchResumeToken' field",
            !expCtx->initialPostBatchResumeToken.isEmpty());

    // The resume stage must come after the check invalidate stage so that the former can determine
    // whether the event that matches the resume token should be followed by an "invalidate" event.
    stages.push_back(DocumentSourceChangeStreamCheckInvalidate::create(expCtx, spec));

    // We must always check that the shard is capable of resuming from the specified point.
    stages.push_back(DocumentSourceChangeStreamCheckResumability::create(expCtx, spec));

    // If 'showExpandedEvents' is NOT set, add a filter that returns only classic change events.
    if (!spec.getShowExpandedEvents()) {
        stages.push_back(DocumentSourceMatch::create(
            change_stream_filter::getMatchFilterForClassicOperationTypes(), expCtx));
    }

    return stages;
}

boost::optional<Document> legacyLookupPreImage(boost::intrusive_ptr<ExpressionContext> pExpCtx,
                                               const Document& preImageId) {
    // We need the oplog's UUID for lookup, so obtain the collection info via MongoProcessInterface.
    auto localOplogInfo = pExpCtx->mongoProcessInterface->getCollectionOptions(
        pExpCtx->opCtx, NamespaceString::kRsOplogNamespace);

    // Extract the UUID from the collection information. We should always have a valid uuid here.
    auto oplogUUID = invariantStatusOK(UUID::parse(localOplogInfo["uuid"]));

    // Look up the pre-image oplog entry using the opTime as the query filter.
    const auto opTime = repl::OpTime::parse(preImageId.toBson());
    auto lookedUpDoc =
        pExpCtx->mongoProcessInterface->lookupSingleDocument(pExpCtx,
                                                             NamespaceString::kRsOplogNamespace,
                                                             oplogUUID,
                                                             Document{opTime.asQuery()},
                                                             boost::none);

    // Return boost::none to signify that we failed to find the pre-image.
    if (!lookedUpDoc) {
        return boost::none;
    }

    // If we had an optime to look up, and we found an oplog entry with that timestamp, then we
    // should always have a valid no-op entry containing a valid, non-empty pre-image document.
    auto opLogEntry = uassertStatusOK(repl::OplogEntry::parse(lookedUpDoc->toBson()));
    tassert(5868901,
            "Oplog entry type must be a no-op",
            opLogEntry.getOpType() == repl::OpTypeEnum::kNoop);
    tassert(5868902,
            "Oplog entry must contait a non-empty pre-image document",
            !opLogEntry.getObject().isEmpty());

    return Document{opLogEntry.getObject().getOwned()};
}

}  // namespace change_stream_legacy
}  // namespace mongo
