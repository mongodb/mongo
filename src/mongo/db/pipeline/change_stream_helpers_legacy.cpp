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

#include "mongo/db/pipeline/document_source_change_stream_add_post_image.h"
#include "mongo/db/pipeline/document_source_change_stream_check_invalidate.h"
#include "mongo/db/pipeline/document_source_change_stream_check_resumability.h"
#include "mongo/db/pipeline/document_source_change_stream_check_topology_change.h"
#include "mongo/db/pipeline/document_source_change_stream_close_cursor.h"
#include "mongo/db/pipeline/document_source_change_stream_ensure_resume_token_present.h"
#include "mongo/db/pipeline/document_source_change_stream_handle_topology_change.h"
#include "mongo/db/pipeline/document_source_change_stream_lookup_pre_image.h"
#include "mongo/db/pipeline/document_source_change_stream_oplog_match.h"
#include "mongo/db/pipeline/document_source_change_stream_transform.h"
#include "mongo/db/pipeline/document_source_change_stream_unwind_transactions.h"
#include "mongo/db/pipeline/expression.h"

namespace mongo {
namespace change_stream_legacy {

std::list<boost::intrusive_ptr<DocumentSource>> buildPipeline(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, DocumentSourceChangeStreamSpec spec) {
    std::list<boost::intrusive_ptr<DocumentSource>> stages;

    const auto userRequestedResumePoint =
        spec.getResumeAfter() || spec.getStartAfter() || spec.getStartAtOperationTime();

    if (!userRequestedResumePoint) {
        // Make sure we update the 'resumeAfter' in the 'spec' so that we serialize the
        // correct resume token when sending it to the shards.
        spec.setResumeAfter(ResumeToken::makeHighWaterMarkToken(
            DocumentSourceChangeStream::getStartTimeForNewStream(expCtx)));
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

    auto resumeToken = DocumentSourceChangeStream::resolveResumeTokenFromSpec(spec);

    // If the user-requested resume point is a high water mark, or if we are running on the shards
    // in a cluster, we must include a DSCSCheckResumability stage.
    if (expCtx->needsMerge ||
        (userRequestedResumePoint && ResumeToken::isHighWaterMarkToken(resumeToken))) {
        stages.push_back(DocumentSourceChangeStreamCheckResumability::create(expCtx, spec));
    }

    // If the pipeline is built on MongoS, then the stage 'DSCSHandleTopologyChange' acts as
    // the split point for the pipline. All stages before this stages will run on shards and all
    // stages after and inclusive of this stage will run on the MongoS.
    if (expCtx->inMongos) {
        stages.push_back(DocumentSourceChangeStreamHandleTopologyChange::create(expCtx));
    }

    // If the resume token is from an event, we must include a DSCSEnsureResumeTokenPresent stage.
    // In a cluster, this will be on mongoS and should not be generated on the shards.
    if (!expCtx->needsMerge && !ResumeToken::isHighWaterMarkToken(resumeToken)) {
        stages.push_back(DocumentSourceChangeStreamEnsureResumeTokenPresent::create(expCtx, spec));
    }

    if (!expCtx->needsMerge) {
        // There should only be one close cursor stage. If we're on the shards and producing input
        // to be merged, do not add a close cursor stage, since the mongos will already have one.
        stages.push_back(DocumentSourceChangeStreamCloseCursor::create(expCtx));

        // We only create a pre-image lookup stage on a non-merging mongoD. We place this stage here
        // so that any $match stages which follow the $changeStream pipeline prefix may be able to
        // skip ahead of the DSCSAddPreImage stage. This allows a whole-db or whole-cluster stream
        // to run on an instance where only some collections have pre-images enabled, so long as the
        // user filters for only those namespaces.
        // TODO SERVER-36941: figure out how to get this to work in a sharded cluster.
        if (spec.getFullDocumentBeforeChange() != FullDocumentBeforeChangeModeEnum::kOff) {
            invariant(!expCtx->inMongos);
            stages.push_back(DocumentSourceChangeStreamAddPreImage::create(expCtx, spec));
        }

        // There should be only one post-image lookup stage. If we're on the shards and producing
        // input to be merged, the lookup is done on the mongos.
        if (spec.getFullDocument() != FullDocumentModeEnum::kDefault) {
            stages.push_back(DocumentSourceChangeStreamAddPostImage::create(expCtx, spec));
        }
    }

    return stages;
}
}  // namespace change_stream_legacy

Value DocumentSourceChangeStreamOplogMatch::serializeLegacy(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    return explain ? Value(Document{{"$_internalOplogMatch"_sd, Document{}}}) : Value();
}

Value DocumentSourceChangeStreamTransform::serializeLegacy(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    tassert(
        5467607,
        str::stream() << "At least one of 'resumeAfter', 'startAfter' or 'startAtOperationTime' "
                         "fields should be present to serialize "
                      << DocumentSourceChangeStreamTransform::kStageName << " stage",
        _changeStreamSpec.getResumeAfter() || _changeStreamSpec.getStartAtOperationTime() ||
            _changeStreamSpec.getStartAfter());

    return Value(Document{{DocumentSourceChangeStream::kStageName, _changeStreamSpec.toBSON()}});
}

Value DocumentSourceChangeStreamCheckInvalidate::serializeLegacy(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    // We only serialize this stage in the context of explain.
    return explain ? Value(DOC(kStageName << Document())) : Value();
}

Value DocumentSourceChangeStreamCheckResumability::serializeLegacy(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    // We only serialize this stage in the context of explain.
    return explain ? Value(DOC(getSourceName()
                               << DOC("resumeToken" << ResumeToken(_tokenFromClient).toDocument())))
                   : Value();
}

Value DocumentSourceChangeStreamUnwindTransaction::serializeLegacy(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    if (explain) {
        tassert(5543800, "nsRegex has not been initialized", _nsRegex != boost::none);
        return Value(Document{{kStageName, Value(Document{{"nsRegex", _nsRegex->pattern()}})}});
    }

    return Value();
}

Value DocumentSourceChangeStreamAddPreImage::serializeLegacy(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    return (explain ? Value{Document{{kStageName, Document()}}} : Value());
}

Value DocumentSourceChangeStreamAddPostImage::serializeLegacy(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    return (explain ? Value{Document{{kStageName, Document()}}} : Value());
}

Value DocumentSourceChangeStreamCheckTopologyChange::serializeLegacy(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    return (explain ? Value{Document{{kStageName, Document()}}} : Value());
}

}  // namespace mongo
