// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/change_stream_check_resumability_stage.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {
/**
 * This class handles the execution part of the change stream ensure resume token present
 * aggregation stage and is part of the execution pipeline. Its construction is based on
 * DocumentSourceChangeStreamEnsureResumeTokenPresent, which handles the optimization part.
 */
class ChangeStreamEnsureResumeTokenPresentStage final : public ChangeStreamCheckResumabilityStage {
public:
    ChangeStreamEnsureResumeTokenPresentStage(
        std::string_view stageName,
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
        ResumeTokenData token);

private:
    GetNextResult doGetNext() final;

    GetNextResult _tryGetNext();

    // Records whether we have observed the token in the resumed stream.
    bool _hasSeenResumeToken = false;
};
}  // namespace mongo::exec::agg
