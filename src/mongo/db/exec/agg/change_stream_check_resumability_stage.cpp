// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/change_stream_check_resumability_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/change_stream_helpers.h"
#include "mongo/db/pipeline/document_source_change_stream_check_resumability.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <compare>
#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceChangeStreamCheckResumabilityToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* changeStreamCheckResumabilityDS =
        dynamic_cast<DocumentSourceChangeStreamCheckResumability*>(documentSource.get());

    tassert(10561303,
            "expected 'DocumentSourceChangeStreamCheckResumability' type",
            changeStreamCheckResumabilityDS);

    return make_intrusive<exec::agg::ChangeStreamCheckResumabilityStage>(
        changeStreamCheckResumabilityDS->kStageName,
        changeStreamCheckResumabilityDS->getExpCtx(),
        changeStreamCheckResumabilityDS->_tokenFromClient);
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(_internalChangeStreamCheckResumability,
                           DocumentSourceChangeStreamCheckResumability::id,
                           documentSourceChangeStreamCheckResumabilityToStageFn)

ChangeStreamCheckResumabilityStage::ChangeStreamCheckResumabilityStage(
    std::string_view stageName,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    ResumeTokenData tokenFromClient)
    : Stage(stageName, pExpCtx), _tokenFromClient(std::move(tokenFromClient)) {}

boost::optional<ResumeTokenData> ChangeStreamCheckResumabilityStage::getClientResumeToken(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    const auto& spec = expCtx->getChangeStreamSpec();
    if (!spec ||
        (!spec->getStartAfter() && !spec->getResumeAfter() && !spec->getStartAtOperationTime())) {
        return boost::none;
    }
    return change_stream::resolveResumeTokenFromSpec(expCtx, *spec);
}

bool ChangeStreamCheckResumabilityStage::isBackfillingScan(
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    const auto& spec = expCtx->getChangeStreamSpec();
    return spec && spec->getBackfillingStartAtOperationTime();
}

ChangeStreamResumeTokenTracker::ChangeStreamResumeTokenTracker(
    boost::optional<ResumeTokenData> tokenFromClient, bool scanStartsBeforeResumePoint)
    : _tokenFromClient(std::move(tokenFromClient)),
      _scanStartsBeforeResumePoint(scanStartsBeforeResumePoint) {}

ChangeStreamResumeTokenTracker::EventPosition ChangeStreamResumeTokenTracker::classify(
    const Document& event) {
    // With no resume token, or once the resume point has been passed, every event is past the
    // resume point. This also covers events without a sort key, which cannot be the resume token.
    if (!_tokenFromClient || _atOrPastResumePoint || !event.metadata().hasSortKey()) {
        return EventPosition::kAfterResumePoint;
    }

    switch (ChangeStreamCheckResumabilityStage::compareAgainstClientResumeToken(
        event, *_tokenFromClient, _scanStartsBeforeResumePoint)) {
        case ChangeStreamCheckResumabilityStage::ResumeStatus::kCheckNextDoc:
            // The event precedes the resume point; more events may still need to be checked.
            return EventPosition::kBeforeResumePoint;
        case ChangeStreamCheckResumabilityStage::ResumeStatus::kFoundToken:
            // The event is the resume point itself. Every later event is past it, so stop
            // comparing.
            _atOrPastResumePoint = true;
            return EventPosition::kAtResumePoint;
        case ChangeStreamCheckResumabilityStage::ResumeStatus::kSurpassedToken:
        case ChangeStreamCheckResumabilityStage::ResumeStatus::kNeedsSplit:
            // The event is at or past the resume point. Stop comparing.
            _atOrPastResumePoint = true;
            return EventPosition::kAfterResumePoint;
    }
    MONGO_UNREACHABLE;
}

GetNextResult ChangeStreamCheckResumabilityStage::doGetNext() {
    if (_resumeStatus == ResumeStatus::kSurpassedToken) {
        return pSource->getNext();
    }

    // Constant for the life of the cursor: whether the scan may produce events that sort before the
    // resume point.
    const bool scanStartsBeforeResumePoint = isBackfillingScan(pExpCtx);

    while (_resumeStatus != ResumeStatus::kSurpassedToken) {
        // The underlying oplog scan will throw OplogQueryMinTsMissing if the minTs in the change
        // stream filter has fallen off the oplog. Catch this and throw a more explanatory error.
        auto nextInput = [this]() {
            try {
                return pSource->getNext();
            } catch (const ExceptionFor<ErrorCodes::OplogQueryMinTsMissing>& ex) {
                LOGV2_ERROR(6663107,
                            "Resume of change stream was not possible",
                            "resumeTimestamp"_attr = _tokenFromClient.clusterTime,
                            "reason"_attr = ex.reason());
                uasserted(
                    ErrorCodes::ChangeStreamHistoryLost,
                    str::stream()
                        << "Resume of change stream was not possible, as the resume point may no "
                           "longer be in the oplog (resumeTimestamp: "
                        << _tokenFromClient.clusterTime << ")");
            }
        }();

        // If we hit EOF or a control event, return it immediately. Control events are exempt from
        // resume suppression: the router must observe every control event, including one whose
        // token equals the resume token. A backfill rebuild starts a fresh cursor at an older scan
        // point and relies on re-observing the control events the old cursor had already seen (for
        // example, to rebuild its accounting state or to retarget shard cursors), so dropping one
        // at the resume token would lose that state.
        if (!nextInput.isAdvanced()) {
            return nextInput;
        }

        // Determine whether the current event sorts before, equal to or after the resume token.
        _resumeStatus = ChangeStreamCheckResumabilityStage::compareAgainstClientResumeToken(
            nextInput.getDocument(), _tokenFromClient, scanStartsBeforeResumePoint);
        switch (_resumeStatus) {
            case ResumeStatus::kCheckNextDoc:
                // If the result was kCheckNextDoc, we are resumable but must swallow this event.
                continue;
            case ResumeStatus::kNeedsSplit:
                // If the result was kNeedsSplit, we found a resume token which matches the client's
                // except for the splitNum attribute. Allow this document to pass through so that
                // the split stage can regenerate the original fragments and their resume tokens.
                return nextInput;
            case ResumeStatus::kSurpassedToken:
                // In this case the resume token wasn't found; it may be on another shard. However,
                // since the oplog scan did not throw, we know that we are resumable. Fall through
                // into the following case and return the document.
                return nextInput;
            case ResumeStatus::kFoundToken:
                // We found the actual token! Return the doc so DSEnsureResumeTokenPresent sees it.
                return nextInput;
        }
    }
    MONGO_UNREACHABLE;
}

// Returns ResumeStatus::kFoundToken if the document retrieved from the resumed pipeline satisfies
// the client's resume token, ResumeStatus::kCheckNextDoc if it is older than the client's token,
// and ResumeToken::kSurpassedToken if it is more recent than the client's resume token, indicating
// that we will never see the token. Return ResumeStatus::kNeedsSplit if we have found the event
// that produced the resume token, but it was split in the original stream.
ChangeStreamCheckResumabilityStage::ResumeStatus
ChangeStreamCheckResumabilityStage::compareAgainstClientResumeToken(
    const Document& eventFromResumedStream,
    const ResumeTokenData& tokenDataFromClient,
    bool scanStartsBeforeResumePoint) {
    const auto tokenFromResumedStream =
        eventFromResumedStream.metadata().getSortKey().getDocument();

    // The clusterTime is the leading field of the encoded resume token, so it can be read without
    // decoding the whole token. Compare it first: any event past the resume point has a different
    // clusterTime and can be rejected without the more expensive full parse.
    const auto clusterTimeFromResumedStream =
        ResumeToken::extractClusterTime(tokenFromResumedStream);

    // The oplog scan normally starts at the resume point, so we never expect an event to be older
    // than the client's resume token. The scan may start before the resume point when the router
    // rebuilds the stream for an end-of-transaction lookback (the scan start is decoupled from the
    // resume token); such an event is swallowed, and every event at or past the resume point is
    // handled below.
    if (clusterTimeFromResumedStream < tokenDataFromClient.clusterTime) {
        tassert(13679800,
                "Expected the stream's resume token clusterTime to not be older than the client's "
                "resume token clusterTime, unless the scan starts before the resume point",
                scanStartsBeforeResumePoint);
        return ResumeStatus::kCheckNextDoc;
    }

    // If the clusterTime is strictly greater than the client's token, this stream cannot be
    // resumed.
    if (clusterTimeFromResumedStream != tokenDataFromClient.clusterTime) {
        return ResumeStatus::kSurpassedToken;
    }

    // The clusterTimes match, so the full token is needed for the remaining comparisons. Compare
    // the two tokens by stream position with the same field order the change stream uses everywhere
    // else, so the ordering is defined in one place (ResumeTokenData::compareStreamPosition).
    const auto tokenDataFromResumedStream = ResumeToken::parse(tokenFromResumedStream).getData();
    const auto position = tokenDataFromResumedStream.compareStreamPosition(tokenDataFromClient);

    // The event carries the client token's event identifier, so it is the resume point. The match
    // is by identifier, independent of the token version: the event at the resume point is emitted
    // in the client's version, but endOfTransaction control events are minted in the stream's
    // version.
    if (position.sameEventIdentifier) {
        // The event may have been split by the original stream; the value of the resume token's
        // fragmentNum field determines the correct return status.
        if (tokenDataFromClient.fragmentNum && !tokenDataFromResumedStream.fragmentNum) {
            return ResumeStatus::kNeedsSplit;
        }
        if (tokenDataFromResumedStream.fragmentNum == tokenDataFromClient.fragmentNum) {
            return ResumeStatus::kFoundToken;
        }
        return tokenDataFromResumedStream.fragmentNum > tokenDataFromClient.fragmentNum
            ? ResumeStatus::kSurpassedToken
            : ResumeStatus::kCheckNextDoc;
    }

    // The event does not carry the client token's event identifier. If it sorts after the client's
    // token then we will never see the resume token. 'unordered' means the identifiers use
    // different versions and cannot be ordered; treat such an event as preceding the resume point
    // rather than risk mistaking it for the resume point.
    if (position.ordering == std::partial_ordering::greater) {
        return ResumeStatus::kSurpassedToken;
    }
    return ResumeStatus::kCheckNextDoc;
}

}  // namespace exec::agg
}  // namespace mongo
