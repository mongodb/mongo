// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/agg/change_stream_check_resumability_stage.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/optional/optional.hpp>
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

    /**
     * Throws a 'ChangeStreamFatalError' if the high water mark of the scan underlying this stage
     * has advanced beyond the client's resume point without us having observed the resume token,
     * which means that the token can never appear in the stream. Called on every EOF so that a
     * stream which produces no events does not have to wait for one in order to fail.
     */
    void _assertResumePointIsStillReachable();

    /**
     * Walks the source chain looking for the stage that owns the resumable scan, and returns the
     * cluster time of that scan's current high water mark, or boost::none if there is none.
     */
    boost::optional<Timestamp> _getSourceHighWaterMarkClusterTime();

    // Records whether we have observed the token in the resumed stream.
    bool _hasSeenResumeToken = false;
};
}  // namespace mongo::exec::agg
