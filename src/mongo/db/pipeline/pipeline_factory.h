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

#include "mongo/db/pipeline/pipeline.h"
#include "mongo/util/modules.h"

namespace mongo {
namespace MONGO_MOD_PUBLIC pipeline_factory {
/**
 * Options for creating a pipeline.
 */
struct MakePipelineOptions {
    bool optimize = true;

    // It is assumed that the pipeline has already been optimized when we create the
    // MakePipelineOptions. If this is not the case, the caller is responsible for setting
    // alreadyOptimized to false.
    bool alreadyOptimized = true;
    bool attachCursorSource = true;

    // When set to true, ensures that default collection collator will be attached to the pipeline.
    // Needs 'attachCursorSource' set to true, in order to be applied.
    bool useCollectionDefaultCollator = false;
    ShardTargetingPolicy shardTargetingPolicy = ShardTargetingPolicy::kAllowed;
    PipelineValidatorCallback validator = nullptr;
    boost::optional<BSONObj> readConcern;
};

/**
 * Parses a Pipeline from a vector of BSONObjs representing DocumentSources. The state of the
 * returned pipeline will depend upon the supplied MakePipelineOptions:
 * - The boolean opts.optimize determines whether the pipeline will be optimized.
 * - If opts.attachCursorSource is false, the pipeline will be returned without attempting to
 * add an initial cursor source.
 */
std::unique_ptr<Pipeline> makePipeline(const std::vector<BSONObj>& rawPipeline,
                                       const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       MakePipelineOptions opts = MakePipelineOptions{});

/**
 * Creates a Pipeline from an AggregateCommandRequest. This preserves any aggregation options
 * set on the aggRequest. The state of the returned pipeline will depend upon the supplied
 * MakePipelineOptions:
 * - The boolean opts.optimize determines whether the pipeline will be optimized.
 * - If opts.attachCursorSource is false, the pipeline will be returned without attempting to
 * add an initial cursor source.
 *
 * This function throws if parsing the pipeline set on aggRequest failed.
 */
std::unique_ptr<Pipeline> makePipeline(AggregateCommandRequest& aggRequest,
                                       const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       boost::optional<BSONObj> shardCursorsSortSpec = boost::none,
                                       const MakePipelineOptions& opts = MakePipelineOptions{});

std::unique_ptr<Pipeline> makePipelineFromViewDefinition(
    const boost::intrusive_ptr<ExpressionContext>& subPipelineExpCtx,
    ResolvedNamespace resolvedNs,
    std::vector<BSONObj> currentPipeline,
    const MakePipelineOptions& opts,
    const NamespaceString& originalNs);

/**
 * Parses a facet sub-pipeline from a vector of raw BSONObjs by sending the raw pipeline through
 * LiteParsed before creating the Pipeline. This skips top-level validation because facet
 * sub-pipelines have different validation requirements than top-level pipelines.
 */
std::unique_ptr<Pipeline> makeFacetPipeline(const std::vector<BSONObj>& rawPipeline,
                                            const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                            PipelineValidatorCallback validator = nullptr);

}  // namespace MONGO_MOD_PUBLIC pipeline_factory
}  // namespace mongo
