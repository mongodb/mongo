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

#include "mongo/db/exec/agg/change_stream_split_large_event_stage.h"

#include "mongo/db/exec/agg/change_stream_check_resumability_stage.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/change_stream_split_event_helpers.h"
#include "mongo/db/pipeline/document_source_change_stream_split_large_event.h"
#include "mongo/util/assert_util.h"

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceChangeStreamSplitLargeEventToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* changeStreamSplitLargeEventDS =
        dynamic_cast<DocumentSourceChangeStreamSplitLargeEvent*>(documentSource.get());

    tassert(10561308,
            "expected 'DocumentSourceChangeStreamSplitLargeEvent' type",
            changeStreamSplitLargeEventDS);

    return make_intrusive<exec::agg::ChangeStreamSplitLargeEventStage>(
        changeStreamSplitLargeEventDS->kStageName,
        changeStreamSplitLargeEventDS->getExpCtx(),
        changeStreamSplitLargeEventDS->_resumeAfterSplit);
}

namespace exec::agg {

namespace {
auto& changeStreamsLargeEventsSplitCounter =
    *MetricBuilder<Counter64>{"changeStreams.largeEventsSplit"};
}

REGISTER_AGG_STAGE_MAPPING(changeStreamSplitLargeEvent,
                           DocumentSourceChangeStreamSplitLargeEvent::id,
                           documentSourceChangeStreamSplitLargeEventToStageFn)

ChangeStreamSplitLargeEventStage::ChangeStreamSplitLargeEventStage(
    StringData stageName,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    boost::optional<ResumeTokenData> resumeAfterSplit)
    : Stage(stageName, pExpCtx), _resumeAfterSplit(std::move(resumeAfterSplit)) {}

GetNextResult ChangeStreamSplitLargeEventStage::doGetNext() {
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
        input.getDocument(),
        this->pExpCtx->getNeedsMerge() || this->pExpCtx->getForPerShardCursor());

    // Make sure to leave some space for the postBatchResumeToken in the cursor response object.
    size_t tokenSize = eventDoc.metadata().getSortKey().getDocument().toBson().objsize();

    // If we are resuming from a split event, check whether this is it. If so, extract the fragment
    // number from which we are resuming. Otherwise, we have already scanned past the resume point,
    // which implies that it may be on another shard. Continue to split this event without skipping.
    size_t skipFragments = _handleResumeAfterSplit(eventDoc, eventBsonSize + tokenSize);

    // Before proceeding, check whether the event is small enough to be returned as-is.
    if (eventBsonSize + tokenSize <= kBSONObjMaxChangeEventSize) {
        return std::move(eventDoc);
    }

    // Split the event into N appropriately-sized fragments.
    _splitEventQueue = change_stream_split_event::splitChangeEvent(
        eventDoc, kBSONObjMaxChangeEventSize - tokenSize, skipFragments);

    // If the user is resuming from a split event but supplied a pipeline which produced a different
    // split, we cannot reproduce the split point. Check if we're about to swallow all fragments.
    uassert(ErrorCodes::ChangeStreamFatalError,
            "Attempted to resume from a split event, but the resumed stream produced a different "
            "split. Ensure that the pipeline used to resume is the same as the original",
            !(skipFragments > 0 && _splitEventQueue.empty()));
    tassert(7182804,
            "Unexpected empty fragment queue after splitting a change stream event",
            !_splitEventQueue.empty());

    // Increment the ServerStatus counter to indicate that we have split a change event.
    changeStreamsLargeEventsSplitCounter.increment();

    // Return the first element from the queue of fragments.
    return _popFromQueue();
}

Document ChangeStreamSplitLargeEventStage::_popFromQueue() {
    auto nextFragment = std::move(_splitEventQueue.front());
    _splitEventQueue.pop();
    return nextFragment;
}

size_t ChangeStreamSplitLargeEventStage::_handleResumeAfterSplit(const Document& eventDoc,
                                                                 size_t eventBsonSize) {
    if (!_resumeAfterSplit) {
        return 0;
    }
    using CSCRS = ChangeStreamCheckResumabilityStage;
    auto resumeStatus = CSCRS::compareAgainstClientResumeToken(eventDoc, *_resumeAfterSplit);
    tassert(7182805,
            "Observed unexpected event before resume point",
            resumeStatus != CSCRS::ResumeStatus::kCheckNextDoc);
    uassert(ErrorCodes::ChangeStreamFatalError,
            "Attempted to resume from a split event fragment, but the event in the resumed "
            "stream was not large enough to be split",
            resumeStatus != CSCRS::ResumeStatus::kNeedsSplit ||
                eventBsonSize > kBSONObjMaxChangeEventSize);
    auto fragmentNum =
        (resumeStatus == CSCRS::ResumeStatus::kNeedsSplit ? *_resumeAfterSplit->fragmentNum : 0);
    _resumeAfterSplit.reset();
    return fragmentNum;
}

}  // namespace exec::agg
}  // namespace mongo
