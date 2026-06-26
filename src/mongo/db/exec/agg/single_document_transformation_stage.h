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
