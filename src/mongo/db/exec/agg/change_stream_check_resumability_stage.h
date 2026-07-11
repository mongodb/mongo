// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/util/modules.h"

#include <string_view>

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

protected:
    GetNextResult doGetNext() override;

    ResumeStatus _resumeStatus = ResumeStatus::kCheckNextDoc;
    const ResumeTokenData _tokenFromClient;
};
}  // namespace mongo::exec::agg
