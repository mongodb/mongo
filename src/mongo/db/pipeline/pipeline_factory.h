// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/pipeline.h"
#include "mongo/util/modules.h"

#include <functional>

namespace mongo {
class LiteParsedPipeline;

namespace [[MONGO_MOD_PUBLIC]] pipeline_factory {
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
    bool desugar = true;

    // When set to true, ensures that default collection collator will be attached to the
    // pipeline. Needs 'attachCursorSource' set to true, in order to be applied.
    bool useCollectionDefaultCollator = false;
    ShardTargetingPolicy shardTargetingPolicy = ShardTargetingPolicy::kAllowed;
    PipelineValidatorCallback validator = nullptr;
    boost::optional<BSONObj> readConcern;

    // Optional caller-supplied hook invoked on the desugared LiteParsedPipeline to resolve/bind
    // involved-namespace views onto its stages.
    std::function<void(LiteParsedPipeline&)> resolveInvolvedNamespacesFn;
};

static const MakePipelineOptions kOptionsMinimal{
    .optimize = false, .alreadyOptimized = false, .attachCursorSource = false, .desugar = false};

static const MakePipelineOptions kDesugarOnly{
    .optimize = false, .alreadyOptimized = false, .attachCursorSource = false, .desugar = true};

/**
 * Factory functions for creating Pipeline objects from various input formats.
 *
 * All makePipeline() overloads accept MakePipelineOptions to control pipeline behavior:
 * - opts.optimize: If true, the pipeline will be optimized before being returned.
 * - opts.attachCursorSource: If false, the pipeline will be returned without attempting to
 *   add an initial cursor source.
 * - opts.alreadyOptimized: Indicates whether the pipeline has already been optimized. This
 *   affects validation rules applied to the pipeline.
 * - opts.desugar: If true, the pipeline will be desugared before being returned.
 * - Other options control shard targeting, collation, read concern, and validation callbacks.
 */

/**
 * Parses a Pipeline from a BSONElement that must be an array of BSONObj stages. The BSONElement
 * is converted to a vector of BSON objects and then delegated to the vector overload.
 */
std::unique_ptr<Pipeline> makePipeline(BSONElement rawPipelineElement,
                                       const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       MakePipelineOptions opts = MakePipelineOptions{});

/**
 * Parses a Pipeline from a vector of BSON objects.
 */
std::unique_ptr<Pipeline> makePipeline(const std::vector<BSONObj>& rawPipeline,
                                       const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       MakePipelineOptions opts = MakePipelineOptions{});

/**
 * Creates a Pipeline from an AggregateCommandRequest. This preserves any aggregation options
 * set on the aggRequest.
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
 * If 'resolvedNs' refers to a view with a non-simple default collation, builds the corresponding
 * collator and installs it on 'expCtx'. No-op for collections and simple-collation views.
 */
void applyViewDefaultCollation(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                               const ResolvedNamespace& resolvedNs);


/**
 * StageParams-input sibling of makePipelineFromViewDefinitionLPP(). Builds a sub-pipeline from
 * pre-computed StageParams rather than a LiteParsedPipeline. Unlike the LPP overload, this
 * function never stitches the view onto the pipeline: StageParams are produced by the LiteParsed
 * layer, which has already applied view resolution, so stitching is structurally unnecessary.
 */
std::unique_ptr<Pipeline> makePipelineFromViewDefinitionStageParams(
    const boost::intrusive_ptr<ExpressionContext>& subPipelineExpCtx,
    const ResolvedNamespace& resolvedNs,
    StageParamsPipeline stageParams,
    const std::vector<BSONObj>& rawPipeline,
    const NamespaceString& userNss,
    const MakePipelineOptions& opts);

/**
 * Parses a facet sub-pipeline from a vector of raw BSONObjs by sending the raw pipeline through
 * LiteParsed before creating the Pipeline. This skips top-level validation because facet
 * sub-pipelines have different validation requirements than top-level pipelines.
 */
std::unique_ptr<Pipeline> makeFacetPipeline(const std::vector<BSONObj>& rawPipeline,
                                            const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                            PipelineValidatorCallback validator = nullptr);

}  // namespace pipeline_factory
}  // namespace mongo
