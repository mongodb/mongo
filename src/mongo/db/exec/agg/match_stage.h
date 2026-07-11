// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/match_processor.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {
namespace exec {
namespace agg {

class MatchStage final : public Stage {

public:
    MatchStage(std::string_view stageName,
               const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
               const std::shared_ptr<MatchProcessor>& matchProcessor,
               bool isTextQuery);

    bool isEOF() const final {
        return pSource && pSource->isEOF();
    }

    Document getExplainOutput(const query_shape::SerializationOptions& opts =
                                  query_shape::SerializationOptions{}) const final;

private:
    GetNextResult doGetNext() override;

    std::shared_ptr<MatchProcessor> _matchProcessor;

    // Tracks memory used while evaluating the match expression. Reports to the operation-wide
    // tracker so all stages contribute to the operation memory total.
    SimpleMemoryUsageTracker _memoryTracker;

    // Pre-built context passed to every match expression evaluation. tracker points to
    // _memoryTracker when expression memory tracking is enabled, and is null otherwise.
    // Both fields are stable for the stage's lifetime.
    EvaluationContext _expressionEvalCtx;
};

}  // namespace agg
}  // namespace exec
}  // namespace mongo
