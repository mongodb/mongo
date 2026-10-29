// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/change_stream_check_resumability_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/change_stream_helpers.h"
#include "mongo/db/pipeline/document_source_change_stream_check_resumability.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

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

ChangeStreamResumeTokenTracker::ChangeStreamResumeTokenTracker(
    boost::optional<ResumeTokenData> tokenFromClient)
    : _tokenFromClient(std::move(tokenFromClient)) {}

ChangeStreamResumeTokenTracker::EventPosition ChangeStreamResumeTokenTracker::classify(
    const Document& event) {
    // With no resume token, or once the resume point has been passed, every event is past the
    // resume point. This also covers events without a sort key, which cannot be the resume token.
    if (!_tokenFromClient || _atOrPastResumePoint || !event.metadata().hasSortKey()) {
        return EventPosition::kAfterResumePoint;
    }

    switch (ChangeStreamCheckResumabilityStage::compareAgainstClientResumeToken(
        event, *_tokenFromClient)) {
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

        // If we hit EOF, return it immediately.
        if (!nextInput.isAdvanced()) {
            return nextInput;
        }

        // Determine whether the current event sorts before, equal to or after the resume token.
        _resumeStatus = ChangeStreamCheckResumabilityStage::compareAgainstClientResumeToken(
            nextInput.getDocument(), _tokenFromClient);
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
    const Document& eventFromResumedStream, const ResumeTokenData& tokenDataFromClient) {
    const auto tokenFromResumedStream =
        eventFromResumedStream.metadata().getSortKey().getDocument();

    // The clusterTime is the leading field of the encoded resume token, so it can be read without
    // decoding the whole token. Compare it first: any event past the resume point has a different
    // clusterTime and can be rejected without the more expensive full parse.
    const auto clusterTimeFromResumedStream =
        ResumeToken::extractClusterTime(tokenFromResumedStream);

    // We start the resume with a $gte query on the timestamp, so we never expect it to be lower
    // than our resume token's timestamp.
    tassert(13679800,
            "Expected the stream's resume token clusterTime to not be older than the client's "
            "resume token clusterTime",
            clusterTimeFromResumedStream >= tokenDataFromClient.clusterTime);

    // If the clusterTime differs from the client's token, this stream cannot be resumed.
    if (clusterTimeFromResumedStream != tokenDataFromClient.clusterTime) {
        return ResumeStatus::kSurpassedToken;
    }

    // The clusterTimes match, so the full token is needed for the remaining comparisons.
    auto tokenDataFromResumedStream = ResumeToken::parse(tokenFromResumedStream).getData();

    // If the tokenType exceeds the client token's type, then we have passed the resume token point.
    // This can happen if the client resumes from a synthetic 'high water mark' token from another
    // shard which happens to have the same clusterTime as an actual change on this shard.
    if (tokenDataFromResumedStream.tokenType != tokenDataFromClient.tokenType) {
        return tokenDataFromResumedStream.tokenType > tokenDataFromClient.tokenType
            ? ResumeStatus::kSurpassedToken
            : ResumeStatus::kCheckNextDoc;
    }

    // If the document's 'txnIndex' sorts before that of the client token, we must keep looking.
    if (tokenDataFromResumedStream.txnOpIndex < tokenDataFromClient.txnOpIndex) {
        return ResumeStatus::kCheckNextDoc;
    } else if (tokenDataFromResumedStream.txnOpIndex > tokenDataFromClient.txnOpIndex) {
        // This could happen if the client provided a txnOpIndex of 0, yet the 0th document in the
        // applyOps was irrelevant (meaning it was an operation on a collection or DB not being
        // watched). Signal that we have read beyond the resume token.
        return ResumeStatus::kSurpassedToken;
    }

    // If 'fromInvalidate' exceeds the client's token value, then we have passed the resume point.
    if (tokenDataFromResumedStream.fromInvalidate != tokenDataFromClient.fromInvalidate) {
        return tokenDataFromResumedStream.fromInvalidate ? ResumeStatus::kSurpassedToken
                                                         : ResumeStatus::kCheckNextDoc;
    }

    // It is acceptable for the stream UUID to differ from the client's, if this is a whole-database
    // or cluster-wide stream and we are comparing operations from different shards at the same
    // clusterTime. If the stream UUID sorts after the client's, however, then the stream is not
    // resumable; we are past the point in the stream where the token should have appeared.
    if (tokenDataFromResumedStream.uuid != tokenDataFromClient.uuid) {
        return tokenDataFromResumedStream.uuid > tokenDataFromClient.uuid
            ? ResumeStatus::kSurpassedToken
            : ResumeStatus::kCheckNextDoc;
    }

    // If the eventIdentifier matches exactly, then we have found the resume point. However, this
    // event may have been split by the original stream; we must check the value of the resume
    // token's fragmentNum field to determine the correct return status.
    if (ValueComparator::kInstance.evaluate(tokenDataFromResumedStream.eventIdentifier ==
                                            tokenDataFromClient.eventIdentifier)) {
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

    // At this point, we know that the tokens differ only by eventIdentifier. The status we return
    // will depend on whether the stream token is logically before or after the client token. If the
    // latter, then we will never see the resume token and the stream cannot be resumed.
    return ValueComparator::kInstance.evaluate(tokenDataFromResumedStream.eventIdentifier >
                                               tokenDataFromClient.eventIdentifier)
        ? ResumeStatus::kSurpassedToken
        : ResumeStatus::kCheckNextDoc;
}

}  // namespace exec::agg
}  // namespace mongo
