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

#include "mongo/db/pipeline/pipeline_factory.h"

#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/optimization/optimize.h"
#include "mongo/db/pipeline/search/search_helper_bson_obj.h"
#include "mongo/db/views/resolved_view.h"

namespace mongo::pipeline_factory {
std::unique_ptr<Pipeline> makePipeline(const std::vector<BSONObj>& rawPipeline,
                                       const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       MakePipelineOptions opts) {
    auto pipeline = Pipeline::parse(rawPipeline, expCtx, opts.validator);

    expCtx->initializeReferencedSystemVariables();

    bool alreadyOptimized = opts.alreadyOptimized;

    if (opts.optimize) {
        pipeline_optimization::optimizePipeline(*pipeline);
        alreadyOptimized = true;
    }

    pipeline->validateCommon(alreadyOptimized);

    if (opts.attachCursorSource) {
        // Creating AggregateCommandRequest in order to pass all necessary 'opts' to the
        // preparePipelineForExecution().
        AggregateCommandRequest aggRequest(expCtx->getNamespaceString(),
                                           pipeline->serializeToBson());
        pipeline = expCtx->getMongoProcessInterface()->preparePipelineForExecution(
            expCtx,
            aggRequest,
            std::move(pipeline),
            boost::none /* shardCursorsSortSpec */,
            opts.shardTargetingPolicy,
            std::move(opts.readConcern),
            opts.useCollectionDefaultCollator);
    }

    return pipeline;
}

std::unique_ptr<Pipeline> makePipeline(AggregateCommandRequest& aggRequest,
                                       const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                       boost::optional<BSONObj> shardCursorsSortSpec,
                                       const MakePipelineOptions& opts) {
    tassert(10892201,
            "shardCursorsSortSpec must not be set if attachCursorSource is false.",
            opts.attachCursorSource || shardCursorsSortSpec == boost::none);

    boost::optional<BSONObj> readConcern;
    // If readConcern is set on opts and aggRequest, assert they are equal.
    if (opts.readConcern && aggRequest.getReadConcern()) {
        readConcern = aggRequest.getReadConcern()->toBSONInner();
        tassert(7393501,
                "Read concern on aggRequest and makePipelineOpts must match.",
                opts.readConcern->binaryEqual(*readConcern));
    } else {
        readConcern = aggRequest.getReadConcern() ? aggRequest.getReadConcern()->toBSONInner()
                                                  : opts.readConcern;
    }

    auto pipeline = Pipeline::parse(aggRequest.getPipeline(), expCtx, opts.validator);
    if (opts.optimize) {
        pipeline_optimization::optimizePipeline(*pipeline);
    }

    constexpr bool alreadyOptimized = true;
    pipeline->validateCommon(alreadyOptimized);
    aggRequest.setPipeline(pipeline->serializeToBson());

    if (opts.attachCursorSource) {
        pipeline = expCtx->getMongoProcessInterface()->preparePipelineForExecution(
            expCtx,
            aggRequest,
            std::move(pipeline),
            shardCursorsSortSpec,
            opts.shardTargetingPolicy,
            std::move(readConcern),
            opts.useCollectionDefaultCollator);
    }

    return pipeline;
}

namespace {
std::unique_ptr<Pipeline> viewPipelineHelperForSearch(
    const boost::intrusive_ptr<ExpressionContext>& subPipelineExpCtx,
    ResolvedNamespace resolvedNs,
    std::vector<BSONObj> currentPipeline,
    const MakePipelineOptions& opts,
    const NamespaceString& originalNs) {
    // Search queries on mongot-indexed views behave differently than non-search aggregations on
    // views. When a user pipeline contains a $search/$vectorSearch stage, idLookup will apply the
    // view transforms as part of its subpipeline. In this way, the view stages will always
    // be applied directly after $_internalSearchMongotRemote and before the remaining
    // stages of the user pipeline. This is to ensure the stages following
    // $search/$vectorSearch in the user pipeline will receive the modified documents: when
    // storedSource is disabled, idLookup will retrieve full/unmodified documents during
    // (from the _id values returned by mongot), apply the view's data transforms, and pass
    // said transformed documents through the rest of the user pipeline.
    const ResolvedView resolvedView{resolvedNs.ns, std::move(resolvedNs.pipeline), BSONObj()};
    subPipelineExpCtx->setView(
        boost::make_optional(std::make_pair(originalNs, resolvedView.getPipeline())));

    // return the user pipeline without appending the view stages.
    return makePipeline(currentPipeline, subPipelineExpCtx, opts);
}
}  // namespace

std::unique_ptr<Pipeline> makePipelineFromViewDefinition(
    const boost::intrusive_ptr<ExpressionContext>& subPipelineExpCtx,
    ResolvedNamespace resolvedNs,
    std::vector<BSONObj> currentPipeline,
    const MakePipelineOptions& opts,
    const NamespaceString& originalNs) {

    // Update subpipeline's ExpressionContext with the resolved namespace.
    subPipelineExpCtx->setNamespaceString(resolvedNs.ns);

    if (resolvedNs.pipeline.empty()) {
        return makePipeline(currentPipeline, subPipelineExpCtx, opts);
    }

    if (search_helper_bson_obj::isMongotPipeline(currentPipeline)) {
        return viewPipelineHelperForSearch(
            subPipelineExpCtx, std::move(resolvedNs), std::move(currentPipeline), opts, originalNs);
    }

    auto resolvedPipeline = std::move(resolvedNs.pipeline);
    // When we get a resolved pipeline back, we may not yet have its namespaces available in the
    // expression context, e.g. if the view's pipeline contains a $lookup on another collection.
    LiteParsedPipeline liteParsedPipeline(resolvedNs.ns, resolvedPipeline);
    subPipelineExpCtx->addResolvedNamespaces(liteParsedPipeline.getInvolvedNamespaces());

    resolvedPipeline.reserve(currentPipeline.size() + resolvedPipeline.size());
    resolvedPipeline.insert(resolvedPipeline.end(),
                            std::make_move_iterator(currentPipeline.begin()),
                            std::make_move_iterator(currentPipeline.end()));

    return makePipeline(resolvedPipeline, subPipelineExpCtx, opts);
}
}  // namespace mongo::pipeline_factory
