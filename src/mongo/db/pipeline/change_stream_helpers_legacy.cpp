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

#include "mongo/db/pipeline/document_source_change_stream_close_cursor.h"
#include "mongo/db/pipeline/document_source_change_stream_ensure_resume_token_present.h"
#include "mongo/db/pipeline/document_source_change_stream_oplog_match.h"
#include "mongo/db/pipeline/document_source_change_stream_transform.h"
#include "mongo/db/pipeline/document_source_change_stream_unwind_transactions.h"
#include "mongo/db/pipeline/document_source_check_invalidate.h"
#include "mongo/db/pipeline/document_source_check_resume_token.h"
#include "mongo/db/pipeline/document_source_lookup_change_post_image.h"
#include "mongo/db/pipeline/document_source_lookup_change_pre_image.h"
#include "mongo/db/pipeline/document_source_update_on_add_shard.h"
#include "mongo/db/pipeline/expression.h"

namespace mongo {
namespace change_stream_legacy {

std::list<boost::intrusive_ptr<DocumentSource>> buildPipeline(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const DocumentSourceChangeStreamSpec spec,
    BSONElement rawSpec) {
    std::list<boost::intrusive_ptr<DocumentSource>> stages;
    boost::intrusive_ptr<DocumentSource> resumeStage = nullptr;
    boost::optional<ResumeTokenData> startAfterInvalidate;
    bool showMigrationEvents = spec.getShowMigrationEvents();
    uassert(31123,
            "Change streams from mongos may not show migration events.",
            !(expCtx->inMongos && showMigrationEvents));

    auto resumeAfter = spec.getResumeAfter();
    auto startAfter = spec.getStartAfter();
    if (resumeAfter || startAfter) {
        uassert(50865,
                "Do not specify both 'resumeAfter' and 'startAfter' in a $changeStream stage",
                !startAfter || !resumeAfter);

        const ResumeToken token = resumeAfter ? resumeAfter.get() : startAfter.get();
        const ResumeTokenData tokenData = token.getData();

        // If resuming from an "invalidate" using "startAfter", pass along the resume token data to
        // DocumentSourceCheckInvalidate to signify that another invalidate should not be generated.
        if (startAfter && tokenData.fromInvalidate) {
            startAfterInvalidate = tokenData;
        }

        uassert(ErrorCodes::InvalidResumeToken,
                "Attempting to resume a change stream using 'resumeAfter' is not allowed from an "
                "invalidate notification.",
                !resumeAfter || !tokenData.fromInvalidate);

        // If we are resuming a single-collection stream, the resume token should always contain a
        // UUID unless the token is a high water mark.
        uassert(ErrorCodes::InvalidResumeToken,
                "Attempted to resume a single-collection stream, but the resume token does not "
                "include a UUID.",
                tokenData.uuid || !expCtx->isSingleNamespaceAggregation() ||
                    ResumeToken::isHighWaterMarkToken(tokenData));

        // For a regular resume token, we must ensure that (1) all shards are capable of resuming
        // from the given clusterTime, and (2) that we observe the resume token event in the stream
        // before any event that would sort after it. High water mark tokens, however, do not refer
        // to a specific event; we thus only need to check (1), similar to 'startAtOperationTime'.
        if (expCtx->needsMerge || ResumeToken::isHighWaterMarkToken(tokenData)) {
            resumeStage = DocumentSourceCheckResumability::create(expCtx, tokenData);
        } else {
            resumeStage = DocumentSourceEnsureResumeTokenPresent::create(expCtx, tokenData);
        }
    }

    // If we do not have a 'resumeAfter' starting point, check for 'startAtOperationTime'.
    if (auto startAtOperationTime = spec.getStartAtOperationTime()) {
        uassert(40674,
                "Only one type of resume option is allowed, but multiple were found.",
                !resumeStage);
        resumeStage = DocumentSourceCheckResumability::create(expCtx, *startAtOperationTime);
    }

    auto transformStage = DocumentSourceChangeStreamTransform::createFromBson(rawSpec, expCtx);
    tassert(5467606,
            "'DocumentSourceChangeStreamTransform' stage should populate "
            "'initialPostBatchResumeToken' field",
            !expCtx->initialPostBatchResumeToken.isEmpty());

    // We must always build the DSOplogMatch stage even on mongoS, since our validation logic relies
    // upon the fact that it is always the first stage in the pipeline.
    stages.push_back(DocumentSourceOplogMatch::create(expCtx, showMigrationEvents));

    stages.push_back(DocumentSourceChangeStreamUnwindTransaction::create(expCtx));
    stages.push_back(transformStage);


    // The resume stage must come after the check invalidate stage so that the former can determine
    // whether the event that matches the resume token should be followed by an "invalidate" event.
    stages.push_back(DocumentSourceCheckInvalidate::create(expCtx, startAfterInvalidate));

    // The resume stage 'DocumentSourceCheckResumability' should come before the split point stage
    // 'DocumentSourceUpdateOnAddShard'.
    if (resumeStage &&
        resumeStage->getSourceName() == DocumentSourceCheckResumability::kStageName) {
        stages.push_back(resumeStage);
        resumeStage.reset();
    }

    // If the pipeline is build on MongoS, then the stage 'DocumentSourceUpdateOnAddShard' acts as
    // the split point for the pipline. All stages before this stages will run on shards and all
    // stages after and inclusive of this stage will run on the MongoS.
    if (expCtx->inMongos) {
        stages.push_back(DocumentSourceUpdateOnAddShard::create(expCtx));
    }

    // This resume stage should be 'DocumentSourceEnsureResumeTokenPresent'.
    if (resumeStage) {
        stages.push_back(resumeStage);
    }

    if (!expCtx->needsMerge) {
        if (!feature_flags::gFeatureFlagChangeStreamsOptimization.isEnabledAndIgnoreFCV()) {
            // There should only be one close cursor stage. If we're on the shards and producing
            // input to be merged, do not add a close cursor stage, since the mongos will already
            // have one.
            stages.push_back(DocumentSourceCloseCursor::create(expCtx));
        }

        // We only create a pre-image lookup stage on a non-merging mongoD. We place this stage here
        // so that any $match stages which follow the $changeStream pipeline prefix may be able to
        // skip ahead of the DSLPreImage stage. This allows a whole-db or whole-cluster stream to
        // run on an instance where only some collections have pre-images enabled, so long as the
        // user filters for only those namespaces.
        // TODO SERVER-36941: figure out how to get this to work in a sharded cluster.
        if (spec.getFullDocumentBeforeChange() != FullDocumentBeforeChangeModeEnum::kOff) {
            invariant(!expCtx->inMongos);
            stages.push_back(DocumentSourceLookupChangePreImage::create(expCtx, spec));
        }

        // There should be only one post-image lookup stage.  If we're on the shards and producing
        // input to be merged, the lookup is done on the mongos.
        if (spec.getFullDocument() == FullDocumentModeEnum::kUpdateLookup) {
            stages.push_back(DocumentSourceLookupChangePostImage::create(expCtx));
        }
    }

    return stages;
}
}  // namespace change_stream_legacy

Value DocumentSourceOplogMatch::serializeLegacy(
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

    return Value(Document{{getSourceName(), _changeStreamSpec.toBSON()}});
}

Value DocumentSourceCheckInvalidate::serializeLegacy(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    // We only serialize this stage in the context of explain.
    return explain ? Value(DOC(kStageName << Document())) : Value();
}

Value DocumentSourceCheckResumability::serializeLegacy(
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

Value DocumentSourceLookupChangePreImage::serializeLegacy(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    return (explain ? Value{Document{{kStageName, Document()}}} : Value());
}

Value DocumentSourceLookupChangePostImage::serializeLegacy(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    return (explain ? Value{Document{{kStageName, Document()}}} : Value());
}

}  // namespace mongo
