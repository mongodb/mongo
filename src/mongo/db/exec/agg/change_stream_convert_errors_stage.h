// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {

/**
 * The topmost stage of an opted-in ('showErrorsAsEvents: true') change stream pipeline. It owns
 * the conversion of recoverable failures raised by the stages below it into
 * 'operationType: "error"' events, built from the identity recorded on the ExpressionContext (by
 * the transform stage, or by a stage that held the event back and re-recorded it on release). It
 * also materializes a resume gap stashed below the user stages, and releases an error event that a
 * resume stage threw past the router user stages.
 *
 * Installed above every user stage on each node that executes a change stream pipeline, so an
 * error event is always built above the stages that must not observe it. It is injected
 * post-optimization, so the optimizer never sees it; it is a DocumentSource (rather than an
 * exec-only stage) so it shows up in explain on data-bearing nodes (the router's merge-side
 * explain does not build the cursor, so the router instance does not appear there).
 */
class ChangeStreamConvertErrorsStage final : public Stage {
public:
    ChangeStreamConvertErrorsStage(std::string_view stageName,
                                   const boost::intrusive_ptr<ExpressionContext>& pExpCtx)
        : Stage(stageName, pExpCtx) {}

private:
    GetNextResult doGetNext() final;
};

}  // namespace mongo::exec::agg
