// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(changeStreamSplitLargeEvent,
                                     ChangeStreamSplitLargeEventLiteParsed::parse,
                                     AllowedWithApiStrict::kNeverInVersion1);

REGISTER_DOCUMENT_SOURCE_WITH_STAGE_PARAMS_DEFAULT(changeStreamSplitLargeEvent,
                                                   DocumentSourceChangeStreamSplitLargeEvent,
                                                   ChangeStreamSplitLargeEventStageParams);

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

Value DocumentSourceChangeStreamSplitLargeEvent::serialize(
    const query_shape::SerializationOptions& opts) const {
    return Value(Document{{DocumentSourceChangeStreamSplitLargeEvent::kStageName, Document{}}});
}

StageConstraints DocumentSourceChangeStreamSplitLargeEvent::constraints(
    PipelineSplitState pipeState) const {
    StageConstraints constraints{StreamType::kStreaming,
                                 PositionRequirement::kCustom,
                                 HostTypeRequirement::kTargetedShards,
                                 DiskUseRequirement::kNoDiskUse,
                                 FacetRequirement::kNotAllowed,
                                 TransactionRequirement::kNotAllowed,
                                 LookupRequirement::kNotAllowed,
                                 UnionRequirement::kNotAllowed,
                                 ChangeStreamRequirement::kRequiresChangeStream};

    // The user cannot specify multiple split stages in the pipeline.
    constraints.canAppearOnlyOnceInPipeline = true;
    constraints.consumesLogicalCollectionData = false;
    constraints.outputDependsOnSingleInput = true;
    return constraints;
}

DocumentSource::GetModPathsReturn DocumentSourceChangeStreamSplitLargeEvent::getModifiedPaths()
    const {
    // This stage may modify the entire document.
    return {GetModPathsReturn::Type::kAllPaths, {}, {}};
}

DocumentSourceContainer::iterator DocumentSourceChangeStreamSplitLargeEvent::optimizeAt(
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
