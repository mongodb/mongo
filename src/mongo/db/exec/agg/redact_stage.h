// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/redact_processor.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>

namespace mongo::exec::agg {

class RedactStage final : public Stage {
public:
    RedactStage(std::string_view stageName,
                const boost::intrusive_ptr<ExpressionContext>& expCtx,
                const std::shared_ptr<RedactProcessor>& redactProcessor);

private:
    GetNextResult doGetNext() final;
    std::shared_ptr<RedactProcessor> _redactProcessor;

    // Tracks memory used while evaluating the redact expression. Reports to the operation-wide
    // tracker so all stages contribute to the operation memory total.
    SimpleMemoryUsageTracker _memoryTracker;

    // Pre-built context passed to every redact expression evaluation. tracker points to
    // _memoryTracker when expression memory tracking is enabled, and is null otherwise.
    // Both fields are stable for the stage's lifetime.
    EvaluationContext _expressionEvalCtx;
};

}  // namespace mongo::exec::agg
