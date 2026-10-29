// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/memory_tracking/memory_usage_tracker.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/single_document_transformation_processor.h"
#include "mongo/util/modules.h"

#include <string_view>

namespace mongo {
namespace exec {
namespace agg {

/**
 * This class handles the execution part of the single document transformation aggregation stage and
 * is part of the execution pipeline. Its construction is based on
 * DocumentSourceSingleDocumentTransformation, which handles the optimization part.
 */
class SingleDocumentTransformationStage final : public Stage {
public:
    SingleDocumentTransformationStage(
        std::string_view stageName,
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
        const std::shared_ptr<SingleDocumentTransformationProcessor>& transformationProcessor);

    bool isEOF() const final {
        return pSource && pSource->isEOF();
    }

    Document getExplainOutput(const query_shape::SerializationOptions& opts =
                                  query_shape::SerializationOptions{}) const final;

private:
    GetNextResult doGetNext() final;

    void doDispose() final;

    // CommonStats::stageTypeStr is a non-owning std::string_view. For most exec stages the source
    // name is a static constexpr so the pointer is always valid. SingleDocumentTransformationStage
    // is different because the DocumentSource it is built from (e.g. $replaceRoot, $addFields,
    // $project) stores the name in a std::string member, and getSourceName() returns
    // _name.c_str(). If the DocumentSource is destroyed before the exec stage, as happens in 1:N
    // translation functions that create throwaway DocumentSources to feed buildStage(),
    // stageTypeStr becomes a dangling pointer. Owning a copy here keeps the name valid for the
    // lifetime of the stage.
    std::string _ownedStageName;
    std::shared_ptr<SingleDocumentTransformationProcessor> _transformationProcessor;

    // Tracks memory used while evaluating the transformation expressions. Reports to the
    // operation-wide tracker so all stages contribute to the operation memory total.
    SimpleMemoryUsageTracker _memoryTracker;

    // Pre-built context passed to every transformation expression evaluation. tracker points to
    // _memoryTracker when expression memory tracking is enabled, and is null otherwise. stageName
    // is always set so it can be reported in ExceededMemoryLimit error messages. Both fields are
    // stable for the stage's lifetime.
    EvaluationContext _expressionEvalCtx;
};
}  // namespace agg
}  // namespace exec
}  // namespace mongo
