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

#include "mongo/db/pipeline/change_stream_split_event_helpers.h"
#include "mongo/db/pipeline/document_source_change_stream_ensure_resume_token_present.h"
#include "mongo/db/pipeline/document_source_change_stream_handle_topology_change.h"

namespace mongo {

REGISTER_DOCUMENT_SOURCE(changeStreamSplitLargeEvent,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceChangeStreamSplitLargeEvent::createFromBson,
                         AllowedWithApiStrict::kNeverInVersion1);

boost::intrusive_ptr<DocumentSourceChangeStreamSplitLargeEvent>
DocumentSourceChangeStreamSplitLargeEvent::create(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const DocumentSourceChangeStreamSpec& spec) {
    // If resuming from a split event, pass along the resume token data to DSCSSplitEvent so that it
    // can swallow fragments that precede the actual resume point.
    auto resumeToken = DocumentSourceChangeStream::resolveResumeTokenFromSpec(expCtx, spec);
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
            rawSpec.type() == BSONType::Object && rawSpec.Obj().isEmpty());

    // If there is no change stream spec set on the expression context, then this cannot be a change
    // stream pipeline. Pipeline validation will catch this issue later during parsing.
    if (!expCtx->changeStreamSpec) {
        return new DocumentSourceChangeStreamSplitLargeEvent(expCtx, boost::none);
    }
    return create(expCtx, *expCtx->changeStreamSpec);
}

DocumentSourceChangeStreamSplitLargeEvent::DocumentSourceChangeStreamSplitLargeEvent(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    boost::optional<ResumeTokenData> resumeAfterSplit)
    : DocumentSource(getSourceName(), expCtx), _resumeAfterSplit(std::move(resumeAfterSplit)) {
    tassert(7182801,
            "Expected a split event resume token, but found a non-split token",
            !_resumeAfterSplit || resumeAfterSplit->fragmentNum);
}

Value DocumentSourceChangeStreamSplitLargeEvent::serialize(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    return Value(Document{{DocumentSourceChangeStreamSplitLargeEvent::kStageName, Document{}}});
}

StageConstraints DocumentSourceChangeStreamSplitLargeEvent::constraints(
    Pipeline::SplitState pipeState) const {
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
    return constraints;
}

DocumentSource::GetModPathsReturn DocumentSourceChangeStreamSplitLargeEvent::getModifiedPaths()
    const {
    // This stage may modify the entire document.
    return {GetModPathsReturn::Type::kAllPaths, {}, {}};
}

DocumentSource::GetNextResult DocumentSourceChangeStreamSplitLargeEvent::doGetNext() {
    // If we've already queued up some fragments, return them.
    if (!_splitEventQueue.empty()) {
        return _popFromQueue();
    }

    auto input = pSource->getNext();

    // If the next result is EOF return, it as-is.
    if (!input.isAdvanced()) {
        return input;
    }

    // Process the event to see if it is within the size limit. We have to serialize the document to
    // perform this check, but the helper will also produce a new 'Document' which - if it is small
    // enough to be returned - will not need to be re-serialized by the plan executor.
    auto [eventDoc, eventBsonSize] = change_stream_split_event::processChangeEventBeforeSplit(
        input.releaseDocument(), this->pExpCtx->needsMerge);
    if (eventBsonSize <= kBSONObjMaxChangeEventSize) {
        return std::move(eventDoc);
    }

    // If we are resuming from a split event, check whether this is it. If so, extract the fragment
    // number from which we are resuming. Otherwise, we have already scanned past the resume point,
    // which implies that it may be on another shard. Continue to split this event without skipping.
    using DSCSCR = DocumentSourceChangeStreamCheckResumability;
    size_t skipFirstFragments = 0;
    if (_resumeAfterSplit) {
        auto resumeStatus = DSCSCR::compareAgainstClientResumeToken(eventDoc, *_resumeAfterSplit);
        tassert(7182805,
                "Observed unexpected event before resume point",
                resumeStatus != DSCSCR::ResumeStatus::kCheckNextDoc);
        if (resumeStatus == DSCSCR::ResumeStatus::kNeedsSplit) {
            skipFirstFragments = *_resumeAfterSplit->fragmentNum;
        }
        _resumeAfterSplit.reset();
    }

    // Split the event into N appropriately-sized fragments. Make sure to leave some space for the
    // postBatchResumeToken in the cursor response object.
    size_t tokenSize = eventDoc.metadata().getSortKey().getDocument().toBson().objsize();
    _splitEventQueue = change_stream_split_event::splitChangeEvent(
        eventDoc, kBSONObjMaxChangeEventSize - tokenSize, skipFirstFragments);

    // If the user is resuming from a split event but supplied a pipeline which produced a different
    // split, we cannot reproduce the split point. Check if we're about to swallow all fragments.
    uassert(ErrorCodes::ChangeStreamFatalError,
            "Attempted to resume from a split event, but the resumed stream produced a different "
            "split. Ensure that the pipeline used to resume is the same as the original",
            !(skipFirstFragments > 0 && _splitEventQueue.empty()));
    tassert(7182804,
            "Unexpected empty fragment queue after splitting a change stream event",
            !_splitEventQueue.empty());

    // Return the first element from the queue of fragments.
    return _popFromQueue();
}

Document DocumentSourceChangeStreamSplitLargeEvent::_popFromQueue() {
    auto nextFragment = std::move(_splitEventQueue.front());
    _splitEventQueue.pop();
    return nextFragment;
}

namespace {
// During pipeline optimization, the split stage must move ahead of these change stream stages.
static const std::set<StringData> kStagesToMoveAheadOf = {
    DocumentSourceChangeStreamEnsureResumeTokenPresent::kStageName,
    DocumentSourceChangeStreamHandleTopologyChange::kStageName};
}  // namespace

Pipeline::SourceContainer::iterator DocumentSourceChangeStreamSplitLargeEvent::doOptimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    // Helper to determine whether the iterator has reached its final position in the pipeline.
    // Checks whether $changeStreamSplitLargeEvent should move ahead of the given stage.
    auto shouldMoveAheadOf = [](const auto& stagePtr) {
        return kStagesToMoveAheadOf.count(stagePtr->getSourceName());
    };

    // Find the point in the pipeline that the stage should move to.
    for (auto it = itr; it != container->begin() && shouldMoveAheadOf(*std::prev(it)); --it) {
        std::swap(*it, *std::prev(it));
    }

    // Return an iterator pointing to the next stage to be optimized.
    return std::next(itr);
}

void DocumentSourceChangeStreamSplitLargeEvent::validatePipelinePosition(
    bool alreadyOptimized,
    Pipeline::SourceContainer::const_iterator pos,
    const Pipeline::SourceContainer& container) const {
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
                return kStagesToMoveAheadOf.count(stage->getSourceName());
            }));
};
}  // namespace mongo
