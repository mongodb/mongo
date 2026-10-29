// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/util/builder.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/resume_token.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <queue>
#include <string_view>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {
/**
 * This class handles the execution part of the change stream split large event aggregation
 * stage and is part of the execution pipeline. Its construction is based on
 * DocumentSourceChangeStreamSplitLargeEvent, which handles the optimization part.
 */
class ChangeStreamSplitLargeEventStage final : public Stage {
public:
    static constexpr size_t kBSONObjMaxChangeEventSize = BSONObjMaxInternalSize - (8 * 1024);

    ChangeStreamSplitLargeEventStage(std::string_view stageName,
                                     const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                     boost::optional<ResumeTokenData> resumeAfterSplit);

private:
    GetNextResult doGetNext() final;

    Document _popFromQueue();

    /**
     * In case of resume after split, check whether 'eventDoc' is the split event. If so, extract
     * and return the resume token's fragment number. Otherwise, return zero.
     */
    size_t _handleResumeAfterSplit(const Document& eventDoc, size_t eventBsonSize);

    std::queue<Document> _splitEventQueue;
    boost::optional<ResumeTokenData> _resumeAfterSplit;
};
}  // namespace mongo::exec::agg
