// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/agg/change_stream_ensure_resume_token_present_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/high_water_mark_provider.h"
#include "mongo/db/pipeline/change_stream_start_after_invalidate_info.h"
#include "mongo/db/pipeline/document_source_change_stream_ensure_resume_token_present.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <string_view>

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceChangeStreamEnsureResumeTokenPresentToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* changeStreamEnsureResumeTokenPresentDS =
        dynamic_cast<DocumentSourceChangeStreamEnsureResumeTokenPresent*>(documentSource.get());

    tassert(10561304,
            "expected 'DocumentSourceChangeStreamEnsureResumeTokenPresent' type",
            changeStreamEnsureResumeTokenPresentDS);

    return make_intrusive<exec::agg::ChangeStreamEnsureResumeTokenPresentStage>(
        changeStreamEnsureResumeTokenPresentDS->kStageName,
        changeStreamEnsureResumeTokenPresentDS->getExpCtx(),
        changeStreamEnsureResumeTokenPresentDS->_tokenFromClient);
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(_internalChangeStreamEnsureResumeTokenPresent,
                           DocumentSourceChangeStreamEnsureResumeTokenPresent::id,
                           documentSourceChangeStreamEnsureResumeTokenPresentToStageFn)

ChangeStreamEnsureResumeTokenPresentStage::ChangeStreamEnsureResumeTokenPresentStage(
    std::string_view stageName,
    const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
    ResumeTokenData token)
    : ChangeStreamCheckResumabilityStage(stageName, pExpCtx, std::move(token)) {}

GetNextResult ChangeStreamEnsureResumeTokenPresentStage::doGetNext() {
    // If we have already verified the resume token is present, return the next doc immediately.
    if (_resumeStatus == ResumeStatus::kSurpassedToken) {
        return pSource->getNext();
    }

    auto nextInput = GetNextResult::makeEOF();

    // If we are starting after an 'invalidate' and the invalidating command (e.g. collection drop)
    // occurred at the same clusterTime on more than one shard, then we may see multiple identical
    // resume tokens here. We swallow all of them until the resume status becomes kSurpassedToken.
    while (_resumeStatus != ResumeStatus::kSurpassedToken) {
        // Delegate to DocumentSourceChangeStreamCheckResumability to consume all events up to the
        // token. This will also set '_resumeStatus' to indicate whether we have seen or surpassed
        // the token.
        nextInput = _tryGetNext();

        // If there are no more results, return EOF. We will continue checking for the resume token
        // the next time the getNext method is called. If we hit EOF, then we cannot have surpassed
        // the resume token on this iteration.
        if (!nextInput.isAdvanced()) {
            tassert(9047701,
                    "expecting _resumeStatus to be != kSurpassedToken",
                    _resumeStatus != ResumeStatus::kSurpassedToken);

            if (nextInput.isEOF()) {
                // A stream whose filter is very narrow may not produce any event for an
                // indeterminate amount of time, so waiting for one before we can conclude that the
                // resume point is absent is not good enough. Consult the high water mark of the
                // underlying scan instead, which advances even while the stream is producing no
                // events at all.
                _assertResumePointIsStillReachable();
            }
            return nextInput;
        }

        // When we reach here, we have either found the resume token or surpassed it.
        tassert(9047703,
                "expecting _resumeStatus to be != kCheckNextDoc",
                _resumeStatus != ResumeStatus::kCheckNextDoc);

        // If the resume status is kFoundToken, record the fact that we have seen the token. When we
        // have surpassed the resume token, we will assert that we saw the token before doing so. We
        // cannot simply assert once and then assume we have surpassed the token, because in certain
        // cases we may see 1..N identical tokens and must swallow them all before proceeding.
        _hasSeenResumeToken |= (_resumeStatus == ResumeStatus::kFoundToken);
    }

    // Assert that before surpassing the resume token, we observed the token itself in the stream.
    uassert(
        ErrorCodes::ChangeStreamFatalError,
        str::stream() << "cannot resume stream; the resume token was not found. "
                      << nextInput.getDocument().metadata().getSortKey().getDocument().toString(),
        _hasSeenResumeToken);


    // At this point, we have seen the token and swallowed it. Return the next event to the client.
    tassert(9047704,
            "expecting to have surpassed the resume token",
            _hasSeenResumeToken && _resumeStatus == ResumeStatus::kSurpassedToken);
    return nextInput;
}

void ChangeStreamEnsureResumeTokenPresentStage::_assertResumePointIsStillReachable() {
    // If we have already observed the token then the resume point was present; we are merely
    // waiting for the event which follows it.
    if (_hasSeenResumeToken) {
        return;
    }

    auto highWaterMarkClusterTime = _getSourceHighWaterMarkClusterTime();

    // We can only conclude the resume point is unreachable once a high water mark exists and has
    // moved strictly beyond it. A high water mark at cluster time 'T' only guarantees that events
    // before 'T' have been produced, so events at 'T' itself (including the one we seek) may still
    // be in flight. Without a high water mark this check is a no-op optimization: we fall back to
    // failing the stream once we observe an event which surpasses the resume token. On a router the
    // high water mark only advances once every shard has reported progress, so with an idle shard
    // how quickly we fail depends on that shard emitting periodic noops.
    if (!highWaterMarkClusterTime || *highWaterMarkClusterTime <= _tokenFromClient.clusterTime) {
        return;
    }

    uasserted(ErrorCodes::ChangeStreamFatalError,
              str::stream() << "cannot resume stream; the stream has advanced to cluster time "
                            << highWaterMarkClusterTime->toString()
                            << ", which is past the resume point "
                            << ResumeToken(_tokenFromClient).toDocument().toString());
}

boost::optional<Timestamp>
ChangeStreamEnsureResumeTokenPresentStage::_getSourceHighWaterMarkClusterTime() {
    boost::optional<Timestamp> highWaterMarkClusterTime;
    bool hasCalledHighWaterMarkProvider = false;

    forEachSourceStage([&](Stage& source) {
        if (auto* provider = dynamic_cast<HighWaterMarkProvider*>(&source)) {
            highWaterMarkClusterTime = provider->getHighWaterMarkClusterTime();
            hasCalledHighWaterMarkProvider = true;
            return false;
        }

        // Continue pipeline traversal if we haven't found the source stage.
        return true;
    });

    // Expect that the HighWaterMarkProvider was asked for its cluster time. The returned cluster
    // time from the provider can still be boost::none.
    tassert(9047702,
            "expecting HighWaterMarkProvider to have been called",
            hasCalledHighWaterMarkProvider);
    return highWaterMarkClusterTime;
}

GetNextResult ChangeStreamEnsureResumeTokenPresentStage::_tryGetNext() {
    try {
        return ChangeStreamCheckResumabilityStage::doGetNext();
    } catch (const ExceptionFor<ErrorCodes::ChangeStreamStartAfterInvalidate>& ex) {
        const auto extraInfo = ex.extraInfo<ChangeStreamStartAfterInvalidateInfo>();
        tassert(5779200, "Missing ChangeStreamStartAfterInvalidationInfo on exception", extraInfo);

        DocumentSource::GetNextResult nextInput =
            Document::fromBsonWithMetaData(extraInfo->getStartAfterInvalidateEvent());

        _resumeStatus = ChangeStreamCheckResumabilityStage::compareAgainstClientResumeToken(
            nextInput.getDocument(), _tokenFromClient);

        // This exception should always contain the client-provided resume token.
        tassert(5779201,
                "Client resume token did not match with the resume token on the invalidate event",
                _resumeStatus == ResumeStatus::kFoundToken);

        return nextInput;
    }
}

}  // namespace exec::agg
}  // namespace mongo
