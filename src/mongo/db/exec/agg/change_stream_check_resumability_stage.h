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

#pragma once

#include "mongo/base/string_data.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/resume_token.h"

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

    ChangeStreamCheckResumabilityStage(StringData stageName,
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
