/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_change_stream_split_large_event.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/pipeline/change_stream_helpers.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <iterator>
#include <list>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

REGISTER_DOCUMENT_SOURCE(changeStreamSplitLargeEvent,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceChangeStreamSplitLargeEvent::createFromBson,
                         AllowedWithApiStrict::kNeverInVersion1);
ALLOCATE_DOCUMENT_SOURCE_ID(changeStreamSplitLargeEvent,
                            DocumentSourceChangeStreamSplitLargeEvent::id)

boost::intrusive_ptr<DocumentSourceChangeStreamSplitLargeEvent>
DocumentSourceChangeStreamSplitLargeEvent::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const DocumentSourceChangeStreamSpec& spec) {
    // If resuming from a split event, pass along the resume token data to DSCSSplitEvent so that it
    // can swallow fragments that precede the actual resume point.
    auto resumeToken = change_stream::resolveResumeTokenFromSpec(expCtx, spec);
    auto resumeAfterSplit =
        resumeToken.fragmentNum ? std::move(resumeToken) : boost::optional<ResumeTokenData>{};
    return new DocumentSourceChangeStreamSplitLargeEvent(expCtx, std::move(resumeAfterSplit));
}

boost::intrusive_ptr<DocumentSourceChangeStreamSplitLargeEvent>
DocumentSourceChangeStreamSplitLargeEvent::createFromBson(
    BSONElement rawSpec, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    // We expect an empty object spec for this stage.
    uassert(7182800,
            "$changeStreamSplitLargeEvent spec should be an empty object",
            rawSpec.type() == BSONType::object && rawSpec.Obj().isEmpty());

    // If there is no change stream spec set on the expression context, then this cannot be a change
    // stream pipeline. Pipeline validation will catch this issue later during parsing.
    if (!expCtx->getChangeStreamSpec()) {
        return new DocumentSourceChangeStreamSplitLargeEvent(expCtx, boost::none);
    }
    return create(expCtx, *expCtx->getChangeStreamSpec());
}

DocumentSourceChangeStreamSplitLargeEvent::DocumentSourceChangeStreamSplitLargeEvent(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    boost::optional<ResumeTokenData> resumeAfterSplit)
    : DocumentSource(getSourceName(), expCtx), _resumeAfterSplit(std::move(resumeAfterSplit)) {
    tassert(7182801,
            "Expected a split event resume token, but found a non-split token",
            !_resumeAfterSplit || _resumeAfterSplit->fragmentNum);
}

Value DocumentSourceChangeStreamSplitLargeEvent::serialize(const SerializationOptions& opts) const {
    return Value(Document{{DocumentSourceChangeStreamSplitLargeEvent::kStageName, Document{}}});
}

StageConstraints DocumentSourceChangeStreamSplitLargeEvent::constraints(
    PipelineSplitState pipeState) const {
    StageConstraints constraints{StreamType::kStreaming,
                                 PositionRequirement::kCustom,
                                 HostTypeRequirement::kAnyShard,
                                 DiskUseRequirement::kNoDiskUse,
                                 FacetRequirement::kNotAllowed,
                                 TransactionRequirement::kNotAllowed,
                                 LookupRequirement::kNotAllowed,
                                 UnionRequirement::kNotAllowed,
                                 ChangeStreamRequirement::kRequiresChangeStream};

    // The user cannot specify multiple split stages in the pipeline.
    constraints.canAppearOnlyOnceInPipeline = true;
    constraints.consumesLogicalCollectionData = false;
    return constraints;
}

DocumentSource::GetModPathsReturn DocumentSourceChangeStreamSplitLargeEvent::getModifiedPaths()
    const {
    // This stage may modify the entire document.
    return {GetModPathsReturn::Type::kAllPaths, {}, {}};
}

DocumentSourceContainer::iterator DocumentSourceChangeStreamSplitLargeEvent::doOptimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    // Helper to determine whether the iterator has reached its final position in the pipeline.
    // Checks whether $changeStreamSplitLargeEvent should move ahead of the given stage.
    auto shouldMoveAheadOf = [](const auto& stagePtr) {
        return change_stream_constants::kChangeStreamRouterPipelineStages.contains(
            stagePtr->getSourceName());
    };

    // Find the point in the pipeline that the stage should move to.
    for (auto it = itr; it != container->begin() && shouldMoveAheadOf(*std::prev(it));) {
        // Swap 'it' with the previous stage.
        container->splice(std::prev(it), *container, it);
    }

    // Return an iterator pointing to the next stage to be optimized.
    return std::next(itr);
}

void DocumentSourceChangeStreamSplitLargeEvent::validatePipelinePosition(
    bool alreadyOptimized,
    DocumentSourceContainer::const_iterator pos,
    const DocumentSourceContainer& container) const {

    // The $changeStreamSplitLargeEvent stage must be the final stage in the pipeline before
    // optimization.
    uassert(7182802,
            str::stream() << getSourceName() << " must be the last stage in the pipeline",
            alreadyOptimized || pos == std::prev(container.cend()));

    // The $changeStreamSplitLargeEvent stage must not be after 'kStagesToMoveAheadOf' stages after
    // optimization.
    uassert(7182803,
            str::stream() << getSourceName()
                          << " is at the wrong position in the pipeline after optimization",
            !alreadyOptimized || std::none_of(container.begin(), pos, [](const auto& stage) {
                return change_stream_constants::kChangeStreamRouterPipelineStages.contains(
                    stage->getSourceName());
            }));
};
}  // namespace mongo
