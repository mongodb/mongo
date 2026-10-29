// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {
/**
 * This class handles the execution part of the change stream check resumability aggregation stage
 * and is part of the execution pipeline. Its construction is based on
 * DocumentSourceChangeStreamCheckResumability, which handles the optimization part.
 */
class ChangeStreamCheckResumabilityStage : public Stage {
public:
    // Used to record the results of comparing the token data extracted from documents in the
    // resumed stream against the client's resume token.
    enum class ResumeStatus {
        kFoundToken,      // The stream produced a document satisfying the client resume token.
        kSurpassedToken,  // The stream's latest document is more recent than the resume token.
        kCheckNextDoc,    // The next document produced by the stream may contain the resume token.
        kNeedsSplit       // We found a candidate resume token but the event must be split.
    };

    ChangeStreamCheckResumabilityStage(std::string_view stageName,
                                       const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                       ResumeTokenData tokenFromClient);

    static ResumeStatus compareAgainstClientResumeToken(const Document& eventFromResumedStream,
                                                        const ResumeTokenData& tokenDataFromClient);

    /**
     * Returns the resume token the client used to open or resume the change stream, read from the
     * change stream spec on 'expCtx'. Returns boost::none when 'expCtx' has no change stream spec
     * or the spec carries no resume point (e.g. in unit tests that build stages in isolation).
     */
    static boost::optional<ResumeTokenData> getClientResumeToken(
        const boost::intrusive_ptr<ExpressionContext>& expCtx);

protected:
    GetNextResult doGetNext() override;

    ResumeStatus _resumeStatus = ResumeStatus::kCheckNextDoc;
    const ResumeTokenData _tokenFromClient;
};

/**
 * Tracks where a change stream event sits, relative to the client's resume point, as events flow
 * through the pipeline. Stages that run before DSCSEnsureResumeTokenPresent (in particular the
 * pre-/post-image stages) use it to avoid work on events which the resume point causes to be
 * skipped, and to stop comparing once the resume point has been passed.
 *
 * The result describes the resume point only: kAfterResumePoint means "the resume point does not
 * cause this event to be skipped", not "this event will be delivered to the client". Other filters
 * (namespace, operation type, a user $match, ...) may still drop an event that is past the resume
 * point. Callers must not treat this component as a delivery oracle.
 *
 * If the stream has no resume point at all, every event is reported as kAfterResumePoint without
 * any comparison. A high water mark resume point is classified by the same comparison as an event
 * resume point (see classify()).
 */
class ChangeStreamResumeTokenTracker {
public:
    enum class EventPosition {
        kBeforeResumePoint,  // Precedes the resume point; the resume point causes it to be skipped.
        kAtResumePoint,      // Is the resume point; the resume point causes it to be skipped.
        kAfterResumePoint,   // Is at or past the resume point; the resume point does not skip it.
    };

    explicit ChangeStreamResumeTokenTracker(boost::optional<ResumeTokenData> tokenFromClient);

    /**
     * Classifies 'event' relative to the client's resume point. Events must be classified in the
     * order in which they appear in the stream. Once an event at or past the resume point has been
     * seen, all subsequent events are reported as kAfterResumePoint without further comparison, so
     * at most the events up to and including the resume point are ever compared.
     *
     * A high water mark resume point is classified by this same comparison. A high water mark token
     * identifies a point in time rather than an event, so its token type never matches an event's;
     * every event therefore compares as kSurpassedToken and is reported as kAfterResumePoint. That
     * is correct for a consumer asking "does the resume point skip this event?": no event is
     * skipped on account of a high water mark resume point, because DSCSEnsureResumeTokenPresent is
     * not added to the pipeline for one. The one comparison needed to reach that conclusion is
     * amortized by the latch described above.
     *
     * Like compareAgainstClientResumeToken(), this requires every event to be at or after the
     * resume point's clusterTime. That holds for change streams, whose oplog scan starts at the
     * resume point's clusterTime.
     */
    EventPosition classify(const Document& event);

private:
    boost::optional<ResumeTokenData> _tokenFromClient;
    bool _atOrPastResumePoint = false;
};
}  // namespace mongo::exec::agg
