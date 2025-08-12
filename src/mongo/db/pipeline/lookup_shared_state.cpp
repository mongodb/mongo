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

#include "mongo/db/pipeline/lookup_shared_state.h"

#include "mongo/db/pipeline/document_source_lookup.h"
#include "mongo/db/pipeline/document_source_sequential_document_cache.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

// Explicit instantiations for buildPipeline().
template PipelinePtr LookUpSharedState::buildPipeline<false /*isStreamsEngine*/>(
    const boost::intrusive_ptr<ExpressionContext>& fromExpCtx,
    const Document& inputDoc,
    const boost::intrusive_ptr<ExpressionContext>& expCtx);

template PipelinePtr LookUpSharedState::buildPipeline<true /*isStreamsEngine*/>(
    const boost::intrusive_ptr<ExpressionContext>& fromExpCtx,
    const Document& inputDoc,
    const boost::intrusive_ptr<ExpressionContext>& expCtx);

void lookupPipeValidator(const Pipeline& pipeline) {
    for (const auto& src : pipeline.getSources()) {
        uassert(51047,
                str::stream() << src->getSourceName()
                              << " is not allowed within a $lookup's sub-pipeline",
                src->constraints().isAllowedInLookupPipeline());
    }
}

std::unique_ptr<Pipeline> LookUpSharedState::buildPipelineFromViewDefinition(
    std::vector<BSONObj> serializedPipeline, ResolvedNamespace resolvedNamespace) {
    // We don't want to optimize or attach a cursor source here because we need to update
    // _sharedState->resolvedPipeline so we can reuse it on subsequent calls to getNext(), and
    // we may need to update _sharedState->fieldMatchPipelineIdx as well in the case of a
    // field join.
    MakePipelineOptions opts;
    opts.optimize = false;
    opts.attachCursorSource = false;
    opts.validator = mongo::lookupPipeValidator;

    // Resolve the view definition.
    auto pipeline = Pipeline::makePipelineFromViewDefinition(
        fromExpCtx, resolvedNamespace, std::move(serializedPipeline), opts, fromNs);

    // Store the pipeline with resolved namespaces so that we only trigger this exception on the
    // first input document.
    resolvedPipeline = pipeline->serializeToBson();

    // The index of the field join match stage needs to be set to the length of the view
    // pipeline, as it is no longer the first stage in the resolved pipeline.
    if (localField) {
        fieldMatchPipelineIdx = resolvedNamespace.pipeline.size();
    }

    // Update the expression context with any new namespaces the resolved pipeline has introduced.
    LiteParsedPipeline liteParsedPipeline(resolvedNamespace.ns, resolvedNamespace.pipeline);
    fromExpCtx = makeCopyFromExpressionContext(fromExpCtx,
                                               resolvedNamespace.ns,
                                               resolvedNamespace.uuid,
                                               boost::none,
                                               std::make_pair(fromNs, resolvedNamespace.pipeline));
    fromExpCtx->addResolvedNamespaces(liteParsedPipeline.getInvolvedNamespaces());

    return pipeline;
}

template <bool isStreamsEngine>
PipelinePtr LookUpSharedState::buildPipeline(
    const boost::intrusive_ptr<ExpressionContext>& fromExpCtx,
    const Document& inputDoc,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    if (localField) {
        BSONObj filter =
            !unwindSrc || userPipeline ? BSONObj() : additionalFilter.value_or(BSONObj());
        auto matchStage = DocumentSourceLookUp::makeMatchStageFromInput(
            inputDoc, *localField, foreignField->fullPath(), filter);
        // We've already allocated space for the trailing $match stage in
        // '_sharedState->resolvedPipeline'.
        resolvedPipeline[*fieldMatchPipelineIdx] = matchStage;
    }

    // Copy all 'let' variables into the foreign pipeline's expression context.
    variables.copyToExpCtx(variablesParseState, fromExpCtx.get());
    fromExpCtx->setForcePlanCache(true);

    // Query settings are looked up after parsing and therefore are not populated in the
    // 'fromExpCtx' as part of DocumentSourceLookUp constructor. Assign query settings to the
    // 'fromExpCtx' by copying them from the parent query ExpressionContext.
    fromExpCtx->setQuerySettingsIfNotPresent(expCtx->getQuerySettings());

    // Resolve the 'let' variables to values per the given input document.
    resolveLetVariables(inputDoc, &fromExpCtx->variables, expCtx);

    std::unique_ptr<MongoProcessInterface::ScopedExpectUnshardedCollection>
        expectUnshardedCollectionInScope;

    if (!isForeignShardedLookupAllowed && !fromExpCtx->getInRouter()) {
        // Enforce that the foreign collection must be unsharded for lookup.
        expectUnshardedCollectionInScope =
            fromExpCtx->getMongoProcessInterface()->expectUnshardedCollectionInScope(
                fromExpCtx->getOperationContext(), fromExpCtx->getNamespaceString(), boost::none);
    }

    // If we don't have a cache, build and return the pipeline immediately. We don't support caching
    // for the streams engine.
    if (isStreamsEngine || !cache || cache->isAbandoned()) {
        MakePipelineOptions pipelineOpts;
        pipelineOpts.alreadyOptimized = false;
        pipelineOpts.optimize = true;
        // The streams engine attaches its own remote cursor source, so we don't need to do it here.
        pipelineOpts.attachCursorSource = !isStreamsEngine;
        pipelineOpts.validator = mongo::lookupPipeValidator;
        ;
        // By default, $lookup does not support sharded 'from' collections. The streams engine does
        // not care about sharding, and so it does not allow shard targeting.
        pipelineOpts.shardTargetingPolicy = !isStreamsEngine && isForeignShardedLookupAllowed
            ? ShardTargetingPolicy::kAllowed
            : ShardTargetingPolicy::kNotAllowed;
        try {
            return Pipeline::makePipeline(resolvedPipeline, fromExpCtx, pipelineOpts);
        } catch (const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>& e) {
            // This exception returns the information we need to resolve a sharded view. Update the
            // pipeline with the resolved view definition.
            auto pipeline = buildPipelineFromViewDefinition(
                resolvedPipeline, ResolvedNamespace{e->getNamespace(), e->getPipeline()});

            LOGV2_DEBUG(3254800,
                        3,
                        "$lookup found view definition. ns: {namespace}, pipeline: {pipeline}. New "
                        "$lookup sub-pipeline: {new_pipe}",
                        logAttrs(e->getNamespace()),
                        "pipeline"_attr = Pipeline::serializePipelineForLogging(e->getPipeline()),
                        "new_pipe"_attr = Pipeline::serializePipelineForLogging(resolvedPipeline));

            // We can now safely optimize and reattempt attaching the cursor source.
            pipeline = Pipeline::makePipeline(resolvedPipeline, fromExpCtx, pipelineOpts);

            return pipeline;
        }
    }

    // Construct the basic pipeline without a cache stage. Avoid optimizing here since we need to
    // add the cache first, as detailed below.
    MakePipelineOptions pipelineOpts;
    pipelineOpts.alreadyOptimized = false;
    pipelineOpts.optimize = false;
    pipelineOpts.attachCursorSource = false;
    pipelineOpts.validator = mongo::lookupPipeValidator;
    auto pipeline = Pipeline::makePipeline(resolvedPipeline, fromExpCtx, pipelineOpts);

    // We can store the unoptimized serialization of the pipeline so that if we need to resolve
    // a sharded view later on, and we have a local-foreign field join, we will need to update
    // metadata tracking the position of this join in the _sharedState->resolvedPipeline.
    auto serializedPipeline = pipeline->serializeToBson();

    addCacheStageAndOptimize(*pipeline);

    if (!cache->isServing()) {
        // The cache has either been abandoned or has not yet been built. Attach a cursor.
        auto shardTargetingPolicy = isForeignShardedLookupAllowed
            ? ShardTargetingPolicy::kAllowed
            : ShardTargetingPolicy::kNotAllowed;
        try {
            pipeline = expCtx->getMongoProcessInterface()->preparePipelineForExecution(
                pipeline.release(), shardTargetingPolicy);
        } catch (const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>& e) {
            // This exception returns the information we need to resolve a sharded view. Update the
            // pipeline with the resolved view definition.
            pipeline = buildPipelineFromViewDefinition(
                std::move(serializedPipeline),
                ResolvedNamespace{e->getNamespace(), e->getPipeline()});

            // The serialized pipeline does not have a cache stage, so we will add it back to the
            // pipeline here if the cache has not been abandoned.
            if (cache && !cache->isAbandoned()) {
                addCacheStageAndOptimize(*pipeline);
            }

            LOGV2_DEBUG(3254801,
                        3,
                        "$lookup found view definition. ns: {namespace}, pipeline: {pipeline}. New "
                        "$lookup sub-pipeline: {new_pipe}",
                        logAttrs(e->getNamespace()),
                        "pipeline"_attr = Pipeline::serializePipelineForLogging(e->getPipeline()),
                        "new_pipe"_attr = Pipeline::serializePipelineForLogging(resolvedPipeline));

            // Try to attach the cursor source again.
            pipeline = expCtx->getMongoProcessInterface()->preparePipelineForExecution(
                pipeline.release(), shardTargetingPolicy);
        }
    }

    // If the cache has been abandoned, release it.
    if (cache->isAbandoned()) {
        cache.reset();
    }

    invariant(pipeline);
    return pipeline;
}

/**
 * Method that looks for a DocumentSourceSequentialDocumentCache stage and calls optimizeAt() on
 * it if it has yet to be optimized.
 */
void findAndOptimizeSequentialDocumentCache(Pipeline& pipeline) {
    auto& container = pipeline.getSources();
    auto itr = (&container)->begin();
    while (itr != (&container)->end()) {
        if (auto* sequentialCache =
                dynamic_cast<DocumentSourceSequentialDocumentCache*>(itr->get())) {
            if (!sequentialCache->hasOptimizedPos()) {
                sequentialCache->optimizeAt(itr, &container);
            }
        }
        itr = std::next(itr);
    }
}

void LookUpSharedState::addCacheStageAndOptimize(Pipeline& pipeline) {
    // Adds the cache to the end of the pipeline and calls optimizeContainer which will ensure the
    // stages of the pipeline are in the correct and optimal order, before the cache runs
    // doOptimizeAt. During the optimization process, the cache will either move itself to the
    // correct position in the pipeline, or abandon itself if no suitable cache position exists.
    // Once the cache is finished optimizing, the entire pipeline is optimized.
    //
    // When pipeline optimization is disabled, 'Pipeline::optimizePipeline()' exits early and so the
    // cache would not be placed correctly. So we only add the cache when pipeline optimization is
    // enabled.
    if (auto fp = globalFailPointRegistry().find("disablePipelineOptimization");
        fp && fp->shouldFail()) {
        cache->abandon();
    } else {
        // The cache needs to see the full pipeline in its correct order in order to properly place
        // itself, therefore we are adding it to the end of the pipeline, and calling
        // optimizeContainer on the pipeline to ensure the rest of the pipeline is in its correct
        // order before optimizing the cache.
        // TODO SERVER-84113: We will no longer have separate logic based on if a cache is present
        // in doOptimizeAt(), so we can instead only add and optimize the cache after
        // optimizeContainer is called.
        pipeline.addFinalSource(
            DocumentSourceSequentialDocumentCache::create(fromExpCtx, cache.get_ptr()));

        auto& container = pipeline.getSources();

        Pipeline::optimizeContainer(&container);

        // We want to ensure the cache has been optimized prior to any calls to optimize().
        findAndOptimizeSequentialDocumentCache(pipeline);

        // Optimize the pipeline, with the cache in its correct position if it exists.
        Pipeline::optimizeEachStage(&container);
    }
}

void LookUpSharedState::resolveLetVariables(const Document& localDoc,
                                            Variables* variables,
                                            const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    invariant(variables);

    for (auto& letVar : letVariables) {
        auto value = letVar.expression->evaluate(localDoc, &expCtx->variables);
        variables->setConstantValue(letVar.id, value);
    }
}

}  // namespace mongo
