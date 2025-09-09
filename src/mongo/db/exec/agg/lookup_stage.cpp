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

#include "mongo/db/exec/agg/lookup_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/agg/pipeline_builder.h"
#include "mongo/db/pipeline/document_source_sequential_document_cache.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/stage_memory_limit_knobs/knobs.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/logv2/log.h"


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceLookUpToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* lookupDS = dynamic_cast<DocumentSourceLookUp*>(documentSource.get());

    tassert(10423100, "expected 'DocumentSourceLookUp' type", lookupDS);

    tassert(10423101,
            "if we have not absorbed a $unwind, we cannot absorb a $match",
            !lookupDS->_matchSrc || lookupDS->_unwindSrc);

    return make_intrusive<exec::agg::LookUpStage>(
        lookupDS->kStageName,
        lookupDS->getExpCtx(),
        lookupDS->getSubpipelineExpCtx(),
        lookupDS->getFromNs(),
        lookupDS->getAsField(),
        lookupDS->getLocalField(),
        lookupDS->getForeignField(),
        lookupDS->_fieldMatchPipelineIdx,
        lookupDS->_letVariables,
        lookupDS->_variables,
        lookupDS->_variablesParseState,
        lookupDS->hasPipeline(),
        lookupDS->hasUnwindSrc(),
        lookupDS->hasUnwindSrc() ? lookupDS->getUnwindSource()->indexPath()
                                 : boost::optional<FieldPath>(),
        lookupDS->hasUnwindSrc() && lookupDS->getUnwindSource()->preserveNullAndEmptyArrays(),
        lookupDS->getAdditionalFilter(),
        lookupDS->_sharedState);
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(lookup, DocumentSourceLookUp::id, documentSourceLookUpToStageFn);

LookUpStage::LookUpStage(StringData stageName,
                         const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                         boost::intrusive_ptr<ExpressionContext> fromExpCtx,
                         NamespaceString fromNs,
                         FieldPath as,
                         boost::optional<FieldPath> localField,
                         boost::optional<FieldPath> foreignField,
                         boost::optional<size_t> fieldMatchPipelineIdx,
                         std::vector<LetVariable> letVariables,
                         Variables variables,
                         VariablesParseState variablesParseState,
                         bool hasUserPipeline,
                         bool hasAbsorbedUnwindSrc,
                         boost::optional<FieldPath> unwindIndexPathField,
                         bool unwindPreserveNullsAndEmptyArrays,
                         BSONObj additionalFilter,
                         std::shared_ptr<LookUpSharedState> sharedState)
    : Stage(stageName, pExpCtx),
      _fromExpCtx(std::move(fromExpCtx)),
      _fromNs(std::move(fromNs)),
      _as(std::move(as)),
      _localField(std::move(localField)),
      _foreignField(std::move(foreignField)),
      _fieldMatchPipelineIdx(std::move(fieldMatchPipelineIdx)),
      _letVariables(std::move(letVariables)),
      _variables(std::move(variables)),
      _variablesParseState(std::move(variablesParseState)),
      _hasUserPipeline(hasUserPipeline),
      _hasAbsorbedUnwindSrc(hasAbsorbedUnwindSrc),
      _unwindIndexPathField(std::move(unwindIndexPathField)),
      _unwindPreserveNullsAndEmptyArrays(unwindPreserveNullsAndEmptyArrays),
      _additionalFilter(additionalFilter.getOwned()),
      _sharedState(std::move(sharedState)) {
    if (!hasLocalFieldForeignFieldJoin()) {
        // When local/foreignFields are included, we cannot enable the cache because the $match
        // is a correlated prefix that will not be detected. Here, local/foreignFields are absent,
        // so we enable the cache.
        _cache = std::make_shared<SequentialDocumentCache>(
            loadMemoryLimit(StageMemoryLimit::DocumentSourceLookupCacheSizeBytes));
    }
};

void LookUpStage::detachFromOperationContext() {
    if (_sharedState->execPipeline) {
        // We have a pipeline we're going to be executing across multiple calls to getNext(), so we
        // use Pipeline::detachFromOperationContext() to take care of updating
        // '_fromExpCtx->getOperationContext()'.
        tassert(10713704,
                "expecting '_sharedState->execPipeline' to be initialized when "
                "'_sharedState->pipeline' is initialized",
                _sharedState->execPipeline);
        _sharedState->execPipeline->detachFromOperationContext();
        _sharedState->pipeline->detachFromOperationContext();
        tassert(10713705,
                "expecting _fromExpCtx->getOperationContext() == nullptr",
                _fromExpCtx->getOperationContext() == nullptr);
    }
    if (_fromExpCtx) {
        _fromExpCtx->setOperationContext(nullptr);
    }
    if (_sharedState->resolvedIntrospectionPipeline) {
        _sharedState->resolvedIntrospectionPipeline->detachFromOperationContext();
    }
}

void LookUpStage::reattachToOperationContext(OperationContext* opCtx) {
    if (_sharedState->execPipeline) {
        // We have a pipeline we're going to be executing across multiple calls to getNext(), so we
        // use Pipeline::reattachToOperationContext() to take care of updating
        // '_fromExpCtx->getOperationContext()'.
        tassert(10713708,
                "expecting '_sharedState->execPipeline' to be initialized when "
                "'_sharedState->pipeline' is initialized",
                _sharedState->execPipeline);
        _sharedState->execPipeline->reattachToOperationContext(opCtx);
        _sharedState->pipeline->reattachToOperationContext(opCtx);
        tassert(10713709,
                "expecting _fromExpCtx->getOperationContext() == opCtx",
                _fromExpCtx->getOperationContext() == opCtx);
    }
    if (_fromExpCtx) {
        _fromExpCtx->setOperationContext(opCtx);
    }
    if (_sharedState->resolvedIntrospectionPipeline) {
        _sharedState->resolvedIntrospectionPipeline->reattachToOperationContext(opCtx);
    }
}

bool LookUpStage::validateOperationContext(const OperationContext* opCtx) const {
    if (getContext()->getOperationContext() != opCtx ||
        (_fromExpCtx && _fromExpCtx->getOperationContext() != opCtx)) {
        return false;
    }
    if (_sharedState->execPipeline &&
        !_sharedState->execPipeline->validateOperationContext(opCtx)) {
        return false;
    }
    if (_sharedState->resolvedIntrospectionPipeline &&
        !_sharedState->resolvedIntrospectionPipeline->validateOperationContext(opCtx)) {
        return false;
    }

    return true;
}

bool LookUpStage::usedDisk() const {
    return _sharedState->execPipeline && _sharedState->execPipeline->usedDisk();
}

Document LookUpStage::getExplainOutput(const SerializationOptions& opts) const {
    auto doc = MutableDocument(Stage::getExplainOutput(opts));

    const PlanSummaryStats& stats = _stats.planSummaryStats;
    doc["totalDocsExamined"] = Value(static_cast<long long>(stats.totalDocsExamined));
    doc["totalKeysExamined"] = Value(static_cast<long long>(stats.totalKeysExamined));
    doc["collectionScans"] = Value(stats.collectionScans);
    std::vector<Value> indexesUsedVec;
    std::transform(stats.indexesUsed.begin(),
                   stats.indexesUsed.end(),
                   std::back_inserter(indexesUsedVec),
                   [](const std::string& idx) -> Value { return Value(idx); });
    doc["indexesUsed"] = Value{std::move(indexesUsedVec)};

    return doc.freeze();
}

GetNextResult LookUpStage::doGetNext() {
    if (_hasAbsorbedUnwindSrc) {
        return unwindResult();
    }

    auto nextInput = pSource->getNext();
    if (!nextInput.isAdvanced()) {
        return nextInput;
    }

    auto inputDoc = nextInput.releaseDocument();


    std::unique_ptr<mongo::Pipeline> pipeline;
    std::unique_ptr<Pipeline> execPipeline;
    try {
        pipeline = buildPipeline(_fromExpCtx, inputDoc);
        execPipeline = exec::agg::buildPipeline(pipeline->freeze());
        LOGV2_DEBUG(
            9497000, 5, "Built pipeline", "pipeline"_attr = pipeline->serializeForLogging());
    } catch (const ExceptionFor<ErrorCategory::StaleShardVersionError>& ex) {
        // If lookup on a sharded collection is disallowed and the foreign collection is sharded,
        // throw a custom exception.
        if (auto staleInfo = ex.extraInfo<StaleConfigInfo>(); staleInfo &&
            staleInfo->getVersionWanted() &&
            staleInfo->getVersionWanted() != ShardVersion::UNSHARDED()) {
            uassert(3904800,
                    "Cannot run $lookup with a sharded foreign collection in a transaction",
                    foreignShardedLookupAllowed());
        }
        throw;
    }

    std::vector<Value> results;
    long long objsize = 0;
    const auto maxBytes = internalLookupStageIntermediateDocumentMaxSizeBytes.load();

    LOGV2_DEBUG(9497001, 5, "Beginning to iterate sub-pipeline");
    while (auto result = execPipeline->getNext()) {
        long long safeSum = 0;
        bool hasOverflowed = overflow::add(objsize, result->getApproximateSize(), &safeSum);
        uassert(4568,
                str::stream() << "Total size of documents in " << _fromNs.coll()
                              << " matching pipeline's $lookup stage exceeds " << maxBytes
                              << " bytes",

                !hasOverflowed && objsize <= maxBytes);
        objsize = safeSum;
        results.emplace_back(std::move(*result));
    }
    execPipeline->accumulatePlanSummaryStats(_stats.planSummaryStats);

    _stats.planSummaryStats.usedDisk = _stats.planSummaryStats.usedDisk || execPipeline->usedDisk();

    MutableDocument output(std::move(inputDoc));
    output.setNestedField(_as, Value(std::move(results)));
    return output.freeze();
}

void LookUpStage::doDispose() {
    if (_sharedState->execPipeline) {
        _sharedState->execPipeline->accumulatePlanSummaryStats(_stats.planSummaryStats);
        _sharedState->execPipeline->reattachToOperationContext(pExpCtx->getOperationContext());
        _sharedState->execPipeline->dispose();
        _sharedState->execPipeline.reset();
    }
    if (_sharedState->pipeline) {
        _sharedState->pipeline.reset();
    }
}

std::unique_ptr<mongo::Pipeline> LookUpStage::buildPipelineFromViewDefinition(
    std::vector<BSONObj> serializedPipeline, ResolvedNamespace resolvedNamespace) {
    // We don't want to optimize or attach a cursor source here because we need to update
    // _sharedState->resolvedPipeline so we can reuse it on subsequent calls to getNext(), and we
    // may need to update _fieldMatchPipelineIdx as well in the case of a field join.
    MakePipelineOptions opts;
    opts.optimize = false;
    opts.attachCursorSource = false;
    opts.validator = mongo::lookupPipeValidator;

    // Resolve the view definition.
    auto pipeline = mongo::Pipeline::makePipelineFromViewDefinition(
        _fromExpCtx, resolvedNamespace, std::move(serializedPipeline), opts, _fromNs);

    // Store the pipeline with resolved namespaces so that we only trigger this exception on the
    // first input document.
    _sharedState->resolvedPipeline = pipeline->serializeToBson();

    // The index of the field join match stage needs to be set to the length of the view
    // pipeline, as it is no longer the first stage in the resolved pipeline.
    if (hasLocalFieldForeignFieldJoin()) {
        _fieldMatchPipelineIdx = resolvedNamespace.pipeline.size();
    }

    // Update the expression context with any new namespaces the resolved pipeline has introduced.
    LiteParsedPipeline liteParsedPipeline(resolvedNamespace.ns, resolvedNamespace.pipeline);
    _fromExpCtx =
        makeCopyFromExpressionContext(_fromExpCtx,
                                      resolvedNamespace.ns,
                                      resolvedNamespace.uuid,
                                      boost::none,
                                      std::make_pair(_fromNs, resolvedNamespace.pipeline));
    _fromExpCtx->addResolvedNamespaces(liteParsedPipeline.getInvolvedNamespaces());

    return pipeline;
}

template <bool isStreamsEngine>
std::unique_ptr<mongo::Pipeline> LookUpStage::buildPipeline(
    const boost::intrusive_ptr<ExpressionContext>& fromExpCtx, const Document& inputDoc) {
    if (hasLocalFieldForeignFieldJoin()) {
        auto matchStage = DocumentSourceLookUp::makeMatchStageFromInput(
            inputDoc, *_localField, _foreignField->fullPath(), _additionalFilter);
        // We've already allocated space for the trailing $match stage in
        // '_sharedState->resolvedPipeline'.
        _sharedState->resolvedPipeline[*_fieldMatchPipelineIdx] = matchStage;
    }

    // Copy all 'let' variables into the foreign pipeline's expression context.
    _variables.copyToExpCtx(_variablesParseState, fromExpCtx.get());
    fromExpCtx->setForcePlanCache(true);

    // Query settings are looked up after parsing and therefore are not populated in the
    // 'fromExpCtx' as part of DocumentSourceLookUp constructor. Assign query settings to the
    // 'fromExpCtx' by copying them from the parent query ExpressionContext.
    fromExpCtx->setQuerySettingsIfNotPresent(getContext()->getQuerySettings());

    // Resolve the 'let' variables to values per the given input document.
    resolveLetVariables(inputDoc, &fromExpCtx->variables);

    std::unique_ptr<MongoProcessInterface::ScopedExpectUnshardedCollection>
        expectUnshardedCollectionInScope;

    const auto allowForeignShardedColl = foreignShardedLookupAllowed();
    if (!allowForeignShardedColl && !fromExpCtx->getInRouter()) {
        // Enforce that the foreign collection must be unsharded for lookup.
        expectUnshardedCollectionInScope =
            fromExpCtx->getMongoProcessInterface()->expectUnshardedCollectionInScope(
                fromExpCtx->getOperationContext(), fromExpCtx->getNamespaceString(), boost::none);
    }

    // If we don't have a cache, build and return the pipeline immediately. We don't support caching
    // for the streams engine.
    if (isStreamsEngine || !_cache || _cache->isAbandoned()) {
        MakePipelineOptions pipelineOpts;
        pipelineOpts.alreadyOptimized = false;
        pipelineOpts.optimize = true;
        // The streams engine attaches its own remote cursor source, so we don't need to do it here.
        pipelineOpts.attachCursorSource = !isStreamsEngine;
        pipelineOpts.validator = mongo::lookupPipeValidator;
        // By default, $lookup does not support sharded 'from' collections. The streams engine does
        // not care about sharding, and so it does not allow shard targeting.
        pipelineOpts.shardTargetingPolicy = !isStreamsEngine && allowForeignShardedColl
            ? ShardTargetingPolicy::kAllowed
            : ShardTargetingPolicy::kNotAllowed;
        try {
            return mongo::Pipeline::makePipeline(
                _sharedState->resolvedPipeline, fromExpCtx, pipelineOpts);
        } catch (const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>& e) {
            // This exception returns the information we need to resolve a sharded view. Update the
            // pipeline with the resolved view definition.
            auto pipeline = buildPipelineFromViewDefinition(
                _sharedState->resolvedPipeline,
                ResolvedNamespace{e->getNamespace(), e->getPipeline()});

            LOGV2_DEBUG(
                3254800,
                3,
                "$lookup found view definition. ns: {namespace}, pipeline: {pipeline}. New "
                "$lookup sub-pipeline: {new_pipe}",
                logAttrs(e->getNamespace()),
                "pipeline"_attr = mongo::Pipeline::serializePipelineForLogging(e->getPipeline()),
                "new_pipe"_attr =
                    mongo::Pipeline::serializePipelineForLogging(_sharedState->resolvedPipeline));

            // We can now safely optimize and reattempt attaching the cursor source.
            pipeline = mongo::Pipeline::makePipeline(
                _sharedState->resolvedPipeline, fromExpCtx, pipelineOpts);

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
    auto pipeline =
        mongo::Pipeline::makePipeline(_sharedState->resolvedPipeline, fromExpCtx, pipelineOpts);

    // We can store the unoptimized serialization of the pipeline so that if we need to resolve
    // a sharded view later on, and we have a local-foreign field join, we will need to update
    // metadata tracking the position of this join in the _sharedState->resolvedPipeline.
    auto serializedPipeline = pipeline->serializeToBson();

    addCacheStageAndOptimize(*pipeline);

    if (!_cache->isServing()) {
        // The cache has either been abandoned or has not yet been built. Attach a cursor.
        auto shardTargetingPolicy = allowForeignShardedColl ? ShardTargetingPolicy::kAllowed
                                                            : ShardTargetingPolicy::kNotAllowed;
        try {
            pipeline = pExpCtx->getMongoProcessInterface()->preparePipelineForExecution(
                pipeline.release(), shardTargetingPolicy);
        } catch (const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>& e) {
            // This exception returns the information we need to resolve a sharded view. Update the
            // pipeline with the resolved view definition.
            pipeline = buildPipelineFromViewDefinition(
                std::move(serializedPipeline),
                ResolvedNamespace{e->getNamespace(), e->getPipeline()});

            // The serialized pipeline does not have a cache stage, so we will add it back to the
            // pipeline here if the cache has not been abandoned.
            if (_cache && !_cache->isAbandoned()) {
                addCacheStageAndOptimize(*pipeline);
            }

            LOGV2_DEBUG(
                3254801,
                3,
                "$lookup found view definition. ns: {namespace}, pipeline: {pipeline}. New "
                "$lookup sub-pipeline: {new_pipe}",
                logAttrs(e->getNamespace()),
                "pipeline"_attr = mongo::Pipeline::serializePipelineForLogging(e->getPipeline()),
                "new_pipe"_attr =
                    mongo::Pipeline::serializePipelineForLogging(_sharedState->resolvedPipeline));

            // Try to attach the cursor source again.
            pipeline = pExpCtx->getMongoProcessInterface()->preparePipelineForExecution(
                pipeline.release(), shardTargetingPolicy);
        }
    }

    // If the cache has been abandoned, release it.
    if (_cache->isAbandoned()) {
        _cache.reset();
    }

    invariant(pipeline);
    return pipeline;
}

// Explicit instantiations for buildPipeline().
template std::unique_ptr<mongo::Pipeline> LookUpStage::buildPipeline<false /*isStreamsEngine*/>(
    const boost::intrusive_ptr<ExpressionContext>& fromExpCtx, const Document& inputDoc);

template std::unique_ptr<mongo::Pipeline> LookUpStage::buildPipeline<true /*isStreamsEngine*/>(
    const boost::intrusive_ptr<ExpressionContext>& fromExpCtx, const Document& inputDoc);

/**
 * Method that looks for a DocumentSourceSequentialDocumentCache stage and calls optimizeAt() on
 * it if it has yet to be optimized.
 */
void findAndOptimizeSequentialDocumentCache(mongo::Pipeline& pipeline) {
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

void LookUpStage::addCacheStageAndOptimize(mongo::Pipeline& pipeline) {
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
        _cache->abandon();
    } else {
        // The cache needs to see the full pipeline in its correct order in order to properly place
        // itself, therefore we are adding it to the end of the pipeline, and calling
        // optimizeContainer on the pipeline to ensure the rest of the pipeline is in its correct
        // order before optimizing the cache.
        // TODO SERVER-84113: We will no longer have separate logic based on if a cache is present
        // in doOptimizeAt(), so we can instead only add and optimize the cache after
        // optimizeContainer is called.
        pipeline.addFinalSource(DocumentSourceSequentialDocumentCache::create(_fromExpCtx, _cache));

        auto& container = pipeline.getSources();

        mongo::Pipeline::optimizeContainer(&container);

        // We want to ensure the cache has been optimized prior to any calls to optimize().
        findAndOptimizeSequentialDocumentCache(pipeline);

        // Optimize the pipeline, with the cache in its correct position if it exists.
        mongo::Pipeline::optimizeEachStage(&container);
    }
}

GetNextResult LookUpStage::unwindResult() {

    // Loop until we get a document that has at least one match.
    // Note we may return early from this loop if our source stage is exhausted or if the unwind
    // source was asked to return empty arrays and we get a document without a match.
    while (!_sharedState->pipeline || !_nextValue) {
        // Accumulate stats from the pipeline for the previous input, if applicable. This is to
        // avoid missing the accumulation of stats on an early exit (below) if the input (i.e., left
        // side of the lookup) is done.
        if (_sharedState->execPipeline) {
            _sharedState->execPipeline->accumulatePlanSummaryStats(_stats.planSummaryStats);
            _sharedState->execPipeline->reattachToOperationContext(pExpCtx->getOperationContext());
            _sharedState->execPipeline->dispose();
        }

        auto nextInput = pSource->getNext();
        if (!nextInput.isAdvanced()) {
            return nextInput;
        }

        _input = nextInput.releaseDocument();

        _sharedState->pipeline = buildPipeline(_fromExpCtx, *_input);
        _sharedState->execPipeline = exec::agg::buildPipeline(_sharedState->pipeline->freeze());

        // The $lookup stage takes responsibility for disposing of its Pipeline, since it will
        // potentially be used by multiple OperationContexts, and the $lookup stage is part of an
        // outer Pipeline that will propagate dispose() calls before being destroyed.
        _sharedState->execPipeline->dismissDisposal();

        _cursorIndex = 0;
        _nextValue = _sharedState->execPipeline->getNext();

        if (_unwindPreserveNullsAndEmptyArrays && !_nextValue) {
            // There were no results for this cursor, but the $unwind was asked to preserve empty
            // arrays, so we should return a document without the array.
            MutableDocument output(std::move(*_input));
            // Note this will correctly create objects in the prefix of '_as', to act as if we had
            // created an empty array and then removed it.
            output.setNestedField(_as, Value());
            if (_unwindIndexPathField) {
                output.setNestedField(*_unwindIndexPathField, Value(BSONNULL));
            }
            return output.freeze();
        }
    }

    invariant(bool(_input) && bool(_nextValue));
    auto currentValue = *_nextValue;
    _nextValue = _sharedState->execPipeline->getNext();

    // Move input document into output if this is the last or only result, otherwise perform a copy.
    MutableDocument output(_nextValue ? *_input : std::move(*_input));
    output.setNestedField(_as, Value(currentValue));

    if (_unwindIndexPathField) {
        output.setNestedField(*_unwindIndexPathField, Value(_cursorIndex));
    }

    ++_cursorIndex;
    return output.freeze();
}

bool LookUpStage::foreignShardedLookupAllowed() const {
    const auto fcvSnapshot = serverGlobalParams.mutableFCV.acquireFCVSnapshot();
    return !pExpCtx->getOperationContext()->inMultiDocumentTransaction() ||
        gFeatureFlagAllowAdditionalParticipants.isEnabled(fcvSnapshot);
}

void LookUpStage::resolveLetVariables(const Document& localDoc, Variables* variables) {
    invariant(variables);

    for (auto& letVar : _letVariables) {
        auto value = letVar.expression->evaluate(localDoc, &pExpCtx->variables);
        variables->setConstantValue(letVar.id, value);
    }
}

void LookUpStage::reInitializeCache_forTest(size_t maxCacheSizeBytes) {
    invariant(!hasLocalFieldForeignFieldJoin());
    invariant(!_cache || (_cache->isBuilding() && _cache->sizeBytes() == 0));
    _cache = std::make_shared<SequentialDocumentCache>(maxCacheSizeBytes);
}

}  // namespace exec::agg
}  // namespace mongo
