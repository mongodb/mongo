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

namespace {
// Field names in a resume-gap error event.
constexpr std::string_view kEarliestOplogTimestamp{"earliestOplogTimestamp"};
constexpr std::string_view kSkippedClusterTimeRange{"skippedClusterTimeRange"};
constexpr std::string_view kSkippedFrom{"from"};
constexpr std::string_view kSkippedTo{"to"};

bool isErrorEvent(const Document& doc) {
    const Value opType = doc[DocumentSourceChangeStream::kOperationTypeField];
    return opType.getType() == BSONType::string &&
        opType.getStringData() == DocumentSourceChangeStream::kErrorOpType;
}

bool isErrorEventResult(const GetNextResult& result) {
    return result.isAdvanced() && isErrorEvent(result.getDocument());
}

// Builds an operationType:"error" resume-gap event under the client's requested resume token
// 'requested', which it also uses as the sort key. 'earliestOplogTimestamp' is the oldest available
// oplog entry.
Document makeHistoryLostGapEvent(const ResumeTokenData& requested,
                                 Timestamp earliestOplogTimestamp,
                                 boost::optional<ShardId> shardId) {
    const Value token(ResumeToken(requested).toDocument());
    const auto code = ErrorCodes::ChangeStreamHistoryLost;

    MutableDocument errorDoc;
    errorDoc[DocumentSourceChangeStream::kErrorSourceField] =
        Value(toString(ChangeStreamErrorSource::kResumeGap));
    errorDoc[DocumentSourceChangeStream::kErrorCodeField] = Value(static_cast<int>(code));
    errorDoc[DocumentSourceChangeStream::kErrorCodeNameField] =
        Value(std::string(ErrorCodes::errorString(code)));
    errorDoc[DocumentSourceChangeStream::kErrorMessageField] =
        Value(std::string("resume point predates the earliest available oplog entry"));
    errorDoc[kEarliestOplogTimestamp] = Value(earliestOplogTimestamp);
    errorDoc[kSkippedClusterTimeRange] = Value(
        Document{{kSkippedFrom, requested.clusterTime}, {kSkippedTo, earliestOplogTimestamp}});
    if (shardId) {
        errorDoc[DocumentSourceChangeStream::kShardIdField] = Value(shardId->toString());
    }

    MutableDocument event;
    event[DocumentSourceChangeStream::kOperationTypeField] =
        Value(DocumentSourceChangeStream::kErrorOpType);
    event[DocumentSourceChangeStream::kIdField] = token;
    event[DocumentSourceChangeStream::kErrorField] = Value(errorDoc.freeze());
    event[DocumentSourceChangeStream::kClusterTimeField] = Value(requested.clusterTime);

    constexpr bool isSingleElementKey = true;
    event.metadata().setSortKey(token, isSingleElementKey);
    return event.freeze();
}
}  // namespace

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

bool ChangeStreamCheckResumabilityStage::isResumeGapForClientToken(const Document& doc) const {
    if (!isErrorEvent(doc)) {
        return false;
    }
    const auto source =
        doc[DocumentSourceChangeStream::kErrorField][DocumentSourceChangeStream::kErrorSourceField];
    return source.getType() == BSONType::string &&
        source.getStringData() == toString(ChangeStreamErrorSource::kResumeGap) &&
        ValueComparator::kInstance.evaluate(doc[DocumentSourceChangeStream::kIdField] ==
                                            Value(ResumeToken(_tokenFromClient).toDocument()));
}

void ChangeStreamCheckResumabilityStage::failClosedOnUnsplitErrorEvent(
    const Document& /*errorEvent*/) const {
    uasserted(ErrorCodes::ChangeStreamFatalError,
              str::stream() << "Cannot resume from a split change stream event: the resume token "
                               "matches an error event whose token has no fragment number "
                               "(resumeTimestamp: "
                            << _tokenFromClient.clusterTime << ")");
}

GetNextResult ChangeStreamCheckResumabilityStage::_getNextRethrowingScanErrors() {
    // The underlying oplog scan will throw OplogQueryMinTsMissing if the minTs in the change stream
    // filter has fallen off the oplog. Before the resume point is surpassed, catch this and throw a
    // more explanatory error.
    try {
        return pSource->getNext();
    } catch (const ExceptionFor<ErrorCodes::OplogQueryMinTsMissing>& ex) {
        // Once the resume point is surpassed the error is propagated as it always was.
        if (_resumeStatus == ResumeStatus::kSurpassedToken) {
            throw;
        }
        LOGV2_ERROR(6663107,
                    "Resume of change stream was not possible",
                    "resumeTimestamp"_attr = _tokenFromClient.clusterTime,
                    "reason"_attr = ex.reason());
        uasserted(ErrorCodes::ChangeStreamHistoryLost,
                  str::stream() << "Resume of change stream was not possible, as the resume point "
                                   "may no longer be in the oplog (resumeTimestamp: "
                                << _tokenFromClient.clusterTime << ")");
    } catch (const ExceptionFor<ErrorCodes::CappedPositionLost>& ex) {
        if (!change_stream::changeStreamShowsErrorsAsEvents(pExpCtx)) {
            throw;
        }
        // The oplog cursor lost its position because the oplog rolled over. Report a resumable
        // error instead of the non-resumable CappedPositionLost so the driver reopens the stream.
        LOGV2_DEBUG(13732400,
                    3,
                    "Change stream oplog cursor position lost",
                    "resumeTimestamp"_attr = _tokenFromClient.clusterTime,
                    "reason"_attr = ex.reason());
        uasserted(ErrorCodes::RetryChangeStream,
                  str::stream() << "Change stream oplog cursor position was lost, as the oplog may "
                                   "have rolled over (resumeTimestamp: "
                                << _tokenFromClient.clusterTime << ")");
    }
}

bool ChangeStreamCheckResumabilityStage::_hasUnreportedResumeGap() const {
    return change_stream::changeStreamShowsErrorsAsEvents(pExpCtx) &&
        pExpCtx->getChangeStreamOplogFloorAfterRollover() && !_resumePointAccountedFor;
}

void ChangeStreamCheckResumabilityStage::_signalResumeGap(
    boost::optional<GetNextResult> displacedInput) {
    const auto floor = *pExpCtx->getChangeStreamOplogFloorAfterRollover();
    pExpCtx->setPendingChangeStreamResumeGap(makeHistoryLostGapEvent(
        _tokenFromClient,
        floor,
        pExpCtx->getMongoProcessInterface()->getShardId(pExpCtx->getOperationContext())));
    _resumePointAccountedFor = true;
    _inputDisplacedByGap = std::move(displacedInput);
    LOGV2_DEBUG(13732401,
                3,
                "Change stream resume gap built",
                "resumeTimestamp"_attr = _tokenFromClient.clusterTime,
                "earliestOplogTimestamp"_attr = floor);
    uasserted(ErrorCodes::ChangeStreamHistoryLost,
              str::stream() << "Resume point predates the earliest available oplog entry "
                               "(resumeTimestamp: "
                            << _tokenFromClient.clusterTime << ")");
}

GetNextResult ChangeStreamCheckResumabilityStage::_emitInputDisplacedByGap() {
    auto input = std::move(*_inputDisplacedByGap);
    _inputDisplacedByGap = boost::none;

    // Only the event which surpassed the resume point is ever set aside, so it is an advanced
    // result.
    tassert(13732402,
            "Expected the input displaced by a resume gap to be an event",
            input.isAdvanced());

    // The transform recorded the event's identity before the gap signal cleared it, so record it
    // again for failures in the stages above.
    const auto& doc = input.getDocument();
    const auto& metadata = doc.metadata();
    if (!metadata.hasSortKey() || !metadata.isSingleElementKey() ||
        metadata.getSortKey().getType() != BSONType::object) {
        // An event without a resume token sort key cannot be described by an error event.
        pExpCtx->clearActiveChangeStreamEvent();
    } else {
        change_stream::recordActiveEventIdentity(pExpCtx, doc);
    }
    return input;
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
    // Deliver the input a gap displaced before pulling anything further. It bypasses the
    // comparison below since the resume point is already surpassed.
    if (MONGO_unlikely(_inputDisplacedByGap)) {
        return _emitInputDisplacedByGap();
    }

    if (_resumeStatus == ResumeStatus::kSurpassedToken) {
        // Past the resume point the only failure translated is a mid-stream oplog rollover, and
        // only for an opted-in stream. For any other stream this just returns the next result.
        return _getNextRethrowingScanErrors();
    }

    // Constant for the life of the cursor: whether the scan may produce events that sort before the
    // resume point.
    const bool scanStartsBeforeResumePoint = isBackfillingScan(pExpCtx);

    while (_resumeStatus != ResumeStatus::kSurpassedToken) {
        auto nextInput = _getNextRethrowingScanErrors();

        // If we hit EOF or a control event, return it immediately. Control events are exempt from
        // resume suppression: the router must observe every control event, including one whose
        // token equals the resume token. A backfill rebuild starts a fresh cursor at an older scan
        // point and relies on re-observing the control events the old cursor had already seen (for
        // example, to rebuild its accounting state or to retarget shard cursors), so dropping one
        // at the resume token would lose that state. The exception is an EOF once the history was
        // lost and the gap has not been reported yet, which builds the gap first. A tailable pause
        // is not the end of the stream and never builds a gap.
        if (!nextInput.isAdvanced()) {
            if (nextInput.isEOF() && _hasUnreportedResumeGap()) {
                _signalResumeGap(boost::none);
            }
            return nextInput;
        }

        // An error event built on this node or arriving from a shard may carry no sort key, which
        // the comparison below requires. Forward it without changing the resume state. This is
        // guarded by the opt-in rather than asserted: Ensure inherits this loop and sits above the
        // user stages, so a stream which is not opted in can see a user-made 'operationType:
        // "error"' document here, which must be positioned like any other event.
        if (change_stream::changeStreamShowsErrorsAsEvents(pExpCtx) &&
            isErrorEventResult(nextInput)) {
            const auto& errorEvent = nextInput.getDocument();
            if (!errorEvent.metadata().hasSortKey()) {
                // Error events built by this server always carry a sort key, so this is defensive:
                // forward the event rather than fail the stream, leaving the resume state alone.
                return nextInput;
            }
            // A gap for the client's token accounts for the resume point. It is forwarded by the
            // comparison below, once per shard since the status stays kFoundToken.
            if (isResumeGapForClientToken(errorEvent)) {
                _resumePointAccountedFor = true;
            }
        }

        // Determine whether the current event sorts before, equal to or after the resume token.
        _resumeStatus = ChangeStreamCheckResumabilityStage::compareAgainstClientResumeToken(
            nextInput.getDocument(), _tokenFromClient, scanStartsBeforeResumePoint);
        switch (_resumeStatus) {
            case ResumeStatus::kCheckNextDoc:
                // If the result was kCheckNextDoc, we are resumable but must swallow this event.
                continue;
            case ResumeStatus::kNeedsSplit:
                if (change_stream::changeStreamShowsErrorsAsEvents(pExpCtx) &&
                    isErrorEventResult(nextInput)) {
                    // An error event is never split, so the fragments the client's token names
                    // cannot be regenerated from it.
                    failClosedOnUnsplitErrorEvent(nextInput.getDocument());
                }
                // If the result was kNeedsSplit, we found a resume token which matches the client's
                // except for the splitNum attribute. Allow this document to pass through so that
                // the split stage can regenerate the original fragments and their resume tokens.
                return nextInput;
            case ResumeStatus::kSurpassedToken:
                // In this case the resume token wasn't found; it may be on another shard. If the
                // oplog scan recorded that the history was lost, report the gap before this event
                // and set the event aside so it is delivered after the gap. Otherwise the scan did
                // not throw, so we know that we are resumable: return the document.
                if (_hasUnreportedResumeGap()) {
                    _signalResumeGap(std::move(nextInput));
                }
                return nextInput;
            case ResumeStatus::kFoundToken:
                // We found the actual token! Return the doc so DSEnsureResumeTokenPresent sees it.
                _resumePointAccountedFor = true;
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
