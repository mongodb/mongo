// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/pipeline/document_source_union_with.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/exec/agg/pipeline_builder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/extension/host/extension_search_server_status.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/pipeline/document_source_documents.h"
#include "mongo/db/pipeline/document_source_hybrid_scoring_util.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_queue.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/document_source_union_with_gen.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/lite_parsed_union_with.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/pipeline/resolved_namespace.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/pipeline/search/search_helper_bson_obj.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/explain_policy.h"
#include "mongo/db/shard_role/shard_catalog/raw_data_operation.h"
#include "mongo/db/stats/counters.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/str.h"

#include <iterator>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery


namespace mongo {
using namespace std::literals::string_view_literals;

DocumentSourceContainer DocumentSourceUnionWith::createFromStageParams(
    UnionWithStageParams& params, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    // TODO SERVER-121094 This can be removed once hybrid search desugars into the internal hybrid
    // search stage.
    if (hybrid_scoring_util::isHybridSearchPipeline(params.pipeline) || params.isHybridSearch) {
        hybrid_scoring_util::assertForeignCollectionIsNotTimeseries(params.unionNss, expCtx);
    }

    // It is possible to specify a $unionWith with *only* a collection in order to do a
    // COLLSCAN; thus, not every $unionWith stage will have a subpipeline.
    if (params.subpipelineStageParams.has_value()) {
        return {make_intrusive<DocumentSourceUnionWith>(expCtx,
                                                        std::move(params.unionNss),
                                                        std::move(*params.subpipelineStageParams),
                                                        std::move(params.pipeline),
                                                        std::move(params.resolvedBackingNss))};
    }

    // A $unionWith with no user pipeline against a view should just forward the view's LPP
    // directly.
    if (auto view = tryGetPreResolvedNamespace(params.unionNss, expCtx->getResolvedNamespaces())) {
        auto stageParams = view->getViewPipeline().getStageParams();
        return {make_intrusive<DocumentSourceUnionWith>(expCtx,
                                                        std::move(params.unionNss),
                                                        std::move(stageParams),
                                                        std::move(params.pipeline),
                                                        std::move(*view))};
    }


    return {make_intrusive<DocumentSourceUnionWith>(
        expCtx, std::move(params.unionNss), std::move(params.pipeline))};
}

DocumentSourceContainer unionWithStageParamsToDocumentSourceFn(
    const std::unique_ptr<StageParams>& stageParams,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto* typedParams = dynamic_cast<UnionWithStageParams*>(stageParams.get());
    tassert(11786200, "Expected UnionWithStageParams for unionWith stage", typedParams != nullptr);

    // TODO SERVER-121094 Remove when feature flag is removed.
    auto ifrCtx = expCtx->getIfrContext();
    auto hybridSearchFlagEnabled = ifrCtx &&
        ifrCtx->getSavedFlagValue(feature_flags::gFeatureFlagExtensionsInsideHybridSearch);
    if (!hybridSearchFlagEnabled) {
        return {DocumentSourceUnionWith::createFromBson(typedParams->getOriginalBson(), expCtx)};
    }

    // Reject a user-supplied isHybridSearch flag before building from stage params.
    if (auto originalSpec = typedParams->getOriginalBson();
        originalSpec.type() == BSONType::object) {
        hybrid_scoring_util::validateIsHybridSearchNotSetByUser(expCtx,
                                                                originalSpec.embeddedObject());
    }
    return DocumentSourceUnionWith::createFromStageParams(*typedParams, expCtx);
}

ALLOCATE_AND_REGISTER_STAGE_PARAMS(unionWith, UnionWithStageParams)

ALLOCATE_DOCUMENT_SOURCE_ID(unionWith, DocumentSourceUnionWith::id)

namespace {
MONGO_COMPILER_NOINLINE void logShardedViewFound(
    const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>& e,
    const std::vector<BSONObj>& pipeline) {
    LOGV2_DEBUG(4556300,
                3,
                "$unionWith found view definition. ns: {namespace}, pipeline: {pipeline}. New "
                "$unionWith sub-pipeline: {new_pipe}",
                logAttrs(e->getResolvedNamespace()),
                "pipeline"_attr = Value(e->getBsonPipeline()),
                "new_pipe"_attr = pipeline);
}

void assertAllStagesAllowedInUnionWith(const Pipeline& pipeline) {
    for (const auto& src : pipeline.getSources()) {
        uassert(31441,
                str::stream() << src->getSourceName()
                              << " is not allowed within a $unionWith's sub-pipeline",
                src->constraints().isAllowedInUnionPipeline());
    }
};
}  // namespace

UnionWithSharedState::UnionWithSharedState(std::unique_ptr<Pipeline> pipeline,
                                           std::unique_ptr<exec::agg::Pipeline> execPipeline,
                                           ExecutionProgress executionState)
    : _pipeline(std::move(pipeline)),
      _execPipeline(std::move(execPipeline)),
      _executionState(executionState),
      // We must use variables from the sub-pipeline's ExpressionContext, because some extra
      // varialbes might have been defined in makeCopyForSubPipelineFromExpressionContext
      _variables(_pipeline->getContext()->variables),
      _variablesParseState(
          _pipeline->getContext()->variablesParseState.copyWith(_variables.useIdGenerator())) {}


DocumentSourceUnionWith::DocumentSourceUnionWith(
    const DocumentSourceUnionWith& original,
    const boost::intrusive_ptr<ExpressionContext>& newExpCtx)
    : DocumentSource(kStageName, newExpCtx),
      _sharedState(std::make_shared<UnionWithSharedState>(
          original._sharedState->_pipeline->clone(
              newExpCtx
                  ? makeCopyForSubPipelineFromExpressionContext(
                        newExpCtx,
                        newExpCtx->getResolvedNamespace(original._userNss).getResolvedNamespace(),
                        newExpCtx->getResolvedNamespace(original._userNss).getCollUUID())
                  : nullptr),
          nullptr,
          UnionWithSharedState::ExecutionProgress::kIteratingSource)),
      _userNss(original._userNss),
      _userPipeline(original._userPipeline),
      _userPipelineIsHybridSearch(original._userPipelineIsHybridSearch),
      _fromNsIsAView(original._fromNsIsAView) {
    _sharedState->_pipeline->getContext()->setInUnionWith(true);

    tassert(10577700,
            "explain settings are different for $unionWith and its sub-pipeline",
            getExpCtx()->getExplain() == _sharedState->_pipeline->getContext()->getExplain());
}

DocumentSourceUnionWith::DocumentSourceUnionWith(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, std::unique_ptr<Pipeline> pipeline)
    : DocumentSource(kStageName, expCtx) {
    _sharedState = std::make_shared<UnionWithSharedState>(
        std::move(pipeline), nullptr, UnionWithSharedState::ExecutionProgress::kIteratingSource);

    if (!_sharedState->_pipeline->getContext()->getNamespaceString().isOnInternalDb()) {
        globalOpCounters().gotNestedAggregate();
    }
    _sharedState->_pipeline->getContext()->setInUnionWith(true);
    tassert(10577701,
            "explain settings are different for $unionWith and its sub-pipeline",
            getExpCtx()->getExplain() == _sharedState->_pipeline->getContext()->getExplain());
}

DocumentSourceUnionWith::DocumentSourceUnionWith(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    NamespaceString unionNss,
    std::vector<BSONObj> pipeline)
    : DocumentSource(kStageName, expCtx) {
    boost::optional<ResolvedNamespace> resolvedUnionNs;
    try {
        auto resolvedNamespaces = expCtx->getResolvedNamespaces();
        auto it = resolvedNamespaces.find(unionNss);

        if (it != resolvedNamespaces.end()) {
            resolvedUnionNs = it->second;
            _sharedState = std::make_shared<UnionWithSharedState>(
                parsePipelineWithMaybeViewDefinition(expCtx, *resolvedUnionNs, pipeline, unionNss),
                nullptr,
                UnionWithSharedState::ExecutionProgress::kIteratingSource);
        } else {
            // This case only occurs in a sharded context where the database name is the same
            // as the current namespace, and will be resolved in the catch below.
            // We do not need to use the result of 'makePipeline', since this is simply to raise
            // 'CommandOnShardedViewNotSupportedOnMongod', but we do, anyway, for future proofing
            // and to make static analysis tools happy.
            auto shared_pipeline = pipeline_factory::makePipeline(
                pipeline,
                expCtx,
                expCtx->getMongoProcessInterface()->isExpectedToExecuteQueries()
                    ? pipeline_factory::MakePipelineOptions{}
                    : pipeline_factory::kOptionsMinimal);
            _sharedState = std::make_shared<UnionWithSharedState>(
                std::move(shared_pipeline),
                nullptr,
                UnionWithSharedState::ExecutionProgress::kIteratingSource);
        }
    } catch (const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>& e) {
        logShardedViewFound(e, pipeline);
        // The subpipeline targeted a namespace that resolved to a sharded view. A shard returned
        // the view definition via this exception; re-parse the union pipeline with the resolved
        // view folded in.
        resolvedUnionNs = *e.extraInfo<ResolvedNamespace>();
        _sharedState = std::make_shared<UnionWithSharedState>(
            parsePipelineWithMaybeViewDefinition(expCtx, *resolvedUnionNs, pipeline, unionNss),
            nullptr,
            UnionWithSharedState::ExecutionProgress::kIteratingSource);
    }

    if (!_sharedState->_pipeline->getContext()->getNamespaceString().isOnInternalDb()) {
        globalOpCounters().gotNestedAggregate();
    }
    _sharedState->_pipeline->getContext()->setInUnionWith(true);

    // Save state regarding the resolved view, if any, in case we are running explain with
    // 'executionStats' or 'allPlansExecution' on a $unionWith with a view on a mongod. Otherwise we
    // wouldn't be able to see details about the execution of the view pipeline in the explain
    // result.
    if (expCtx->getExplain() &&
        expCtx->getExplain().value() != explain::VerbosityEnum::kQueryPlanner &&
        resolvedUnionNs.has_value() && resolvedUnionNs->isInvolvedNamespaceAView()) {
        _resolvedNsForView = resolvedUnionNs;
    }
    _fromNsIsAView = resolvedUnionNs.has_value() && resolvedUnionNs->isInvolvedNamespaceAView();

    _userNss = std::move(unionNss);
    _userPipeline = std::move(pipeline);
    _userPipelineIsHybridSearch = hybrid_scoring_util::isHybridSearchPipeline(_userPipeline);
}

DocumentSourceUnionWith::DocumentSourceUnionWith(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    NamespaceString unionNss,
    StageParamsPipeline subpipelineStageParams,
    std::vector<BSONObj> userPipeline,
    ResolvedNamespace resolvedBackingNss)
    : DocumentSource(kStageName, expCtx) {
    boost::optional<ResolvedNamespace> resolvedUnionNs;
    try {
        // A view stitched at lite-parse time arrives via resolvedBackingNss; otherwise consult the
        // ExpressionContext (covers plain collections and the $facet BSON-reparse path).
        const bool viewStitched = resolvedBackingNss.isInvolvedNamespaceAView();
        if (viewStitched) {
            resolvedUnionNs = std::move(resolvedBackingNss);
        } else {
            const auto& resolvedNamespaces = expCtx->getResolvedNamespaces();
            auto it = resolvedNamespaces.find(unionNss);
            if (it != resolvedNamespaces.end()) {
                resolvedUnionNs = it->second;
            }
        }

        // $facet reparses its subpipeline from raw BSON, bypassing the bindResolvedNamespace pass
        // so we must apply the view manually in that scenario.
        if (resolvedUnionNs) {
            // TODO SERVER-117260: This fallback can be removed once $facet goes through normal LP
            // view resolution, rather than reparsing from BSON.
            const bool viewNotYetStitched =
                resolvedUnionNs->isInvolvedNamespaceAView() && !viewStitched;
            if (viewNotYetStitched) {
                _sharedState = std::make_shared<UnionWithSharedState>(
                    parsePipelineWithMaybeViewDefinition(
                        expCtx, *resolvedUnionNs, userPipeline, unionNss),
                    nullptr,
                    UnionWithSharedState::ExecutionProgress::kIteratingSource);
            } else {
                _sharedState = std::make_shared<UnionWithSharedState>(
                    parsePipelineFromStageParamsWithMaybeViewDefinition(
                        expCtx,
                        *resolvedUnionNs,
                        std::move(subpipelineStageParams),
                        userPipeline,
                        unionNss),
                    nullptr,
                    UnionWithSharedState::ExecutionProgress::kIteratingSource);
            }
        } else {
            _sharedState = std::make_shared<UnionWithSharedState>(
                Pipeline::parseFromStageParams(
                    std::move(subpipelineStageParams), expCtx, assertAllStagesAllowedInUnionWith),
                nullptr,
                UnionWithSharedState::ExecutionProgress::kIteratingSource);
        }
    } catch (const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>& e) {
        logShardedViewFound(e, userPipeline);
        resolvedUnionNs = *e.extraInfo<ResolvedNamespace>();
        // Fall back to BSON-based parsing for the sharded view case since the view pipeline
        // is discovered dynamically from the exception and needs full re-parsing.
        _sharedState = std::make_shared<UnionWithSharedState>(
            parsePipelineWithMaybeViewDefinition(expCtx, *resolvedUnionNs, userPipeline, unionNss),
            nullptr,
            UnionWithSharedState::ExecutionProgress::kIteratingSource);
    }

    if (!_sharedState->_pipeline->getContext()->getNamespaceString().isOnInternalDb()) {
        globalOpCounters().gotNestedAggregate();
    }
    _sharedState->_pipeline->getContext()->setInUnionWith(true);

    if (expCtx->getExplain() &&
        expCtx->getExplain().value() != explain::VerbosityEnum::kQueryPlanner &&
        resolvedUnionNs.has_value() && resolvedUnionNs->isInvolvedNamespaceAView()) {
        _resolvedNsForView = resolvedUnionNs;
    }
    _fromNsIsAView = resolvedUnionNs.has_value() && resolvedUnionNs->isInvolvedNamespaceAView();

    _userNss = std::move(unionNss);
    _userPipeline = std::move(userPipeline);
    _userPipelineIsHybridSearch = hybrid_scoring_util::isHybridSearchPipeline(_userPipeline);
}

DocumentSourceUnionWith::~DocumentSourceUnionWith() {
    // When in explain command, the sub-pipeline was not disposed in 'doDispose()', so we need to
    // dispose it here.
    if (getExpCtx()->getExplain()) {
        if (_sharedState->_execPipeline) {
            _sharedState->_execPipeline->reattachToOperationContext(
                getExpCtx()->getOperationContext());
            _sharedState->_execPipeline->dispose();
        }
    }
}

boost::intrusive_ptr<DocumentSource> DocumentSourceUnionWith::clone(
    const boost::intrusive_ptr<ExpressionContext>& newExpCtx) const {
    // At this point the ExpressionContext already has info about any resolved namespaces, so there
    // is no need to resolve them again when creating the clone.
    return make_intrusive<DocumentSourceUnionWith>(*this, newExpCtx);
}

boost::intrusive_ptr<DocumentSource> DocumentSourceUnionWith::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    uassert(ErrorCodes::FailedToParse,
            str::stream()
                << "the $unionWith stage specification must be an object or string, but found "
                << typeName(elem.type()),
            elem.type() == BSONType::object || elem.type() == BSONType::string);

    NamespaceString unionNss;
    std::vector<BSONObj> pipeline;
    if (elem.type() == BSONType::string) {
        unionNss = NamespaceStringUtil::deserialize(expCtx->getNamespaceString().dbName(),
                                                    elem.valueStringData());
    } else {
        // The isHybridSearch flag is internal-only: it is set when a desugared hybrid-search
        // sub-pipeline is serialized across the wire, and re-parsed by internal clients. Reject it
        // when a user supplies it directly.
        hybrid_scoring_util::validateIsHybridSearchNotSetByUser(expCtx, elem.embeddedObject());
        auto unionWithSpec =
            UnionWithSpec::parse(elem.embeddedObject(), IDLParserContext(kStageName));
        if (unionWithSpec.getColl()) {
            // If no database specified, use the same database as the current namespace.
            unionNss = NamespaceStringUtil::deserialize(expCtx->getNamespaceString().dbName(),
                                                        *unionWithSpec.getColl());
        } else {
            // if no collection specified, it must have $documents as first field in pipeline
            LiteParsedUnionWith::validateUnionWithCollectionlessPipeline(
                unionWithSpec.getPipeline());
            unionNss = NamespaceString::makeCollectionlessAggregateNSS(
                expCtx->getNamespaceString().dbName());
        }
        pipeline = unionWithSpec.getPipeline().value_or(std::vector<BSONObj>{});
        if (unionWithSpec.getIsHybridSearch() ||
            hybrid_scoring_util::isHybridSearchPipeline(pipeline)) {
            // If there is a hybrid search stage in our pipeline, then we should validate that we
            // are not running on a timeseries collection.
            //
            // If the hybrid search flag is set to true, this request may have
            // come from a mongos that does not know if the collection is a valid collection for
            // hybrid search. Therefore, we must validate it here.
            hybrid_scoring_util::assertForeignCollectionIsNotTimeseries(unionNss, expCtx);
        }
    }
    return make_intrusive<DocumentSourceUnionWith>(
        expCtx, std::move(unionNss), std::move(pipeline));
}

DocumentSourceContainer::iterator DocumentSourceUnionWith::optimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    auto duplicateAcrossUnion = [&](auto&& nextStage) {
        _sharedState->_pipeline->addFinalSource(
            nextStage->clone(_sharedState->_pipeline->getContext()));
        // Apply the same rewrite to the cached pipeline if available.
        //
        // This reads the verbosity directly from the ExpressionContext, which holds the originally
        // requested (possibly V3) verbosity rather than the translated legacy verbosity.
        //
        // TODO SERVER-130529 The V3 verbosities currently map to the same policy as kExecAllPlans
        // (see the transitional rows in explainPolicyFor()), so this predicate is true for every V3
        // mode today. That reproduces the prior behavior exactly: the emit side (serialize()) still
        // depends on the translated legacy verbosity, so any pushed-down stages recorded here for a
        // planner-only V3 mode are ignored. Once the real V3 policies are implemented, the
        // planner-only modes will correctly report hasExecStats() == false here.
        const auto& explainVerbosity = getExpCtx()->getExplain();
        if (explainVerbosity && explainPolicyFor(*explainVerbosity).hasExecStats()) {
            _pushedDownStages.push_back(nextStage->serialize().getDocument().toBson());
        }
        auto newStageItr = container->insert(itr, std::move(nextStage));
        container->erase(std::next(itr));
        return newStageItr == container->begin() ? newStageItr : std::prev(newStageItr);
    };
    if (std::next(itr) != container->end()) {
        if (auto nextMatch = dynamic_cast<DocumentSourceMatch*>((*std::next(itr)).get()))
            return duplicateAcrossUnion(nextMatch);
        else if (auto nextProject = dynamic_cast<DocumentSourceSingleDocumentTransformation*>(
                     (*std::next(itr)).get()))
            return duplicateAcrossUnion(nextProject);
    }
    return std::next(itr);
};

Value DocumentSourceUnionWith::buildUnionWithResult(Value pipelineValue, Value coll) const {
    auto collectionless =
        _sharedState->_pipeline->getContext()->getNamespaceString().isCollectionlessAggregateNS();
    MutableDocument spec;
    if (!collectionless) {
        spec["coll"] = coll;
    }
    spec["pipeline"] = pipelineValue;
    return Value(DOC(getSourceName() << spec.freezeToValue()));
}

void DocumentSourceUnionWith::appendIsHybridSearchFlag(
    MutableDocument& spec, const query_shape::SerializationOptions& opts) const {
    // The isHybridSearch flag is only carried on the shard-dispatch path, never in the explain
    // serialization: an explain-of-a-view spec can be re-parsed on the (non-internal) router,
    // where the flag would fail validateIsHybridSearchNotSetByUser (error 5491300). Mirrors the
    // guard on $lookup's serialization.
    if (_userPipelineIsHybridSearch && !opts.isSerializingForExplain()) {
        spec[hybrid_scoring_util::kIsHybridSearchFlagFieldName] = Value(true);
    }
}

// TODO SERVER-121094: Remove when featureFlagExtensionsInsideHybridSearch is removed.
Value DocumentSourceUnionWith::legacyUnionWithSerialize(
    const query_shape::SerializationOptions& opts) const {
    if (opts.isSerializingForQueryStats()) {
        const auto serializedPipeline =
            pipeline_factory::makePipeline(_userPipeline,
                                           _sharedState->_pipeline->getContext(),
                                           pipeline_factory::kOptionsMinimal)
                ->serializeToBson(opts);
        return buildUnionWithResult(Value(serializedPipeline),
                                    Value(opts.serializeIdentifier(_userNss.coll())));
    } else {
        MutableDocument spec;
        if (!_sharedState->_pipeline->getContext()
                 ->getNamespaceString()
                 .isCollectionlessAggregateNS()) {
            const auto underlyingNss = _sharedState->_pipeline->getContext()->getNamespaceString();
            spec["coll"] = Value(opts.serializeIdentifier(underlyingNss.coll()));
        }
        // Strip $mergeCursors if present (injected by mongos post-dispatch; invalid on re-parse).
        const bool hasMergeCursors = !_sharedState->_pipeline->getSources().empty() &&
            _sharedState->_pipeline->getSources().front()->getSourceName() == "$mergeCursors"sv;
        if (hasMergeCursors) {
            spec["pipeline"] =
                Value(pipeline_factory::makePipeline(_userPipeline,
                                                     _sharedState->_pipeline->getContext(),
                                                     pipeline_factory::kOptionsMinimal)
                          ->serializeToBson(opts));
        } else {
            spec["pipeline"] = Value(_sharedState->_pipeline->serializeToBson(opts));
        }
        appendIsHybridSearchFlag(spec, opts);
        return Value(DOC(getSourceName() << spec.freezeToValue()));
    }
}

Value DocumentSourceUnionWith::serialize(const query_shape::SerializationOptions& opts) const {
    // The coll value used by most serialization paths (explain, default).
    // Query stats uses _userNss instead (see below).
    Value pipelineContextColl{opts.serializeIdentifier(
        _sharedState->_pipeline->getContext()->getNamespaceString().coll())};

    if (opts.isSerializingForExplain()) {
        // When $unionWith is inside a $lookup's pipeline, we cannot independently
        // finalizePipelineAndExplain because the sub-pipeline may reference $lookup let variables
        // that won't be in scope when the explain is forwarded to shards. Fall back to simple
        // serialization; the $lookup handles explaining its own pipeline.
        if (getExpCtx()->getInLookup()) {
            return buildUnionWithResult(Value(_sharedState->_pipeline->serializeToBson(opts)),
                                        pipelineContextColl);
        }

        // There are several different possible states depending on the explain verbosity as well as
        // the other stages in the pipeline:
        //  * If verbosity is queryPlanner, then the sub-pipeline should be untouched and we can
        //  explain it directly.
        //  * If verbosity is execStats or allPlansExecution, then whether or not to explain the
        //  sub-pipeline depends on if we've started reading from it. For instance, there could be a
        //  $limit stage after the $unionWith which results in only reading from the base collection
        //  branch and not the sub-pipeline.
        std::unique_ptr<Pipeline> pipeCopy;
        if (*opts.verbosity == ExplainOptions::Verbosity::kQueryPlanner) {
            pipeCopy = Pipeline::create(_sharedState->_pipeline->getSources(),
                                        _sharedState->_pipeline->getContext());
        } else if (explainPolicyFor(*opts.verbosity).hasExecStats() &&
                   _sharedState->_executionState >
                       UnionWithSharedState::ExecutionProgress::kIteratingSource) {
            std::vector<BSONObj> recoveredPipeline;
            // We've either exhausted the sub-pipeline or at least started iterating it. Use the
            // cached user pipeline and pushed down stages to get the explain output since the
            // '_pipeline' may have been modified for any optimizations or pushdowns into the
            // initial $cursor stage.
            recoveredPipeline.reserve(_userPipeline.size() + _pushedDownStages.size());
            std::move(
                _userPipeline.begin(), _userPipeline.end(), std::back_inserter(recoveredPipeline));
            std::move(_pushedDownStages.begin(),
                      _pushedDownStages.end(),
                      std::back_inserter(recoveredPipeline));
            // We reset the variables to their inital state for another execution.
            // TODO SERVER-94227 we probably don't need to do any validation as part of this parsing
            // pass?
            _sharedState->_variables.copyToExpCtx(_sharedState->_variablesParseState,
                                                  _sharedState->_pipeline->getContext().get());

            // Resolve the view definition, if there is one.
            if (_resolvedNsForView.has_value()) {
                // This takes care of the case where this code is executing on a mongod and we have
                // the full catalog information, so we can resolve the view.
                pipeCopy = parsePipelineWithMaybeViewDefinition(getExpCtx(),
                                                                *_resolvedNsForView,
                                                                std::move(recoveredPipeline),
                                                                _userNss,
                                                                _userPipelineIsHybridSearch);
            } else {
                pipeCopy = pipeline_factory::makePipeline(recoveredPipeline,
                                                          _sharedState->_pipeline->getContext(),
                                                          pipeline_factory::kDesugarOnly);
            }
        } else {
            // The plan does not require reading from the sub-pipeline, so just include the
            // serialization in the explain output.
            return buildUnionWithResult(Value(_sharedState->_pipeline->serializeToBson(opts)),
                                        pipelineContextColl);
        }

        tassert(11282958, "Missing pipeline copy", pipeCopy);

        auto preparePipelineAndExplain = [&](std::unique_ptr<Pipeline> pipeline) {
            _sharedState->_pipeline->getContext()->initializeReferencedSystemVariables();
            return getExpCtx()->getMongoProcessInterface()->finalizePipelineAndExplain(
                std::move(pipeline),
                *opts.verbosity,
                pipeline_optimization::optimizeAndValidatePipeline);
        };

        BSONObj explainLocal = [&] {
            auto serializedPipe = pipeCopy->serializeToBson();
            try {
                return preparePipelineAndExplain(std::move(pipeCopy));
            } catch (const ExceptionFor<ErrorCodes::CommandOnShardedViewNotSupportedOnMongod>& e) {
                logShardedViewFound(e, _sharedState->_pipeline->serializeToBson());
                // This takes care of the case where this code is executing on mongos and we had to
                // get the view pipeline from a shard.
                //
                // Update pipelineContextColl to the resolved underlying collection so that the
                // $unionWith.coll field in the explain output reflects the actual execution
                // namespace rather than the view name.
                pipelineContextColl =
                    Value(opts.serializeIdentifier(e->getResolvedNamespace().coll()));
                auto resolvedPipeline =
                    parsePipelineWithMaybeViewDefinition(getExpCtx(),
                                                         *e.extraInfo<ResolvedNamespace>(),
                                                         std::move(serializedPipe),
                                                         _userNss,
                                                         _userPipelineIsHybridSearch);
                return preparePipelineAndExplain(std::move(resolvedPipeline));
            }
        }();

        LOGV2_DEBUG(4553501, 3, "$unionWith attached cursor to pipeline for explain");
        // We expect this to be an explanation of a pipeline -- there should only be one field.
        tassert(11282957,
                "Expecting pipeline explain to contain exactly 1 field",
                explainLocal.nFields() == 1);

        return buildUnionWithResult(Value(explainLocal.firstElement()), pipelineContextColl);
    } else {
        // TODO SERVER-121094: Remove legacyUnionWithSerialize() and this gate when
        // featureFlagExtensionsInsideHybridSearch is removed.
        auto ifrCtx = getExpCtx()->getIfrContext();
        if (!ifrCtx ||
            !ifrCtx->getSavedFlagValue(feature_flags::gFeatureFlagExtensionsInsideHybridSearch)) {
            return legacyUnionWithSerialize(opts);
        }

        // For query stats, use the original unresolved namespace and re-parse user pipeline.
        if (opts.isSerializingForQueryStats()) {
            const auto serializedPipeline =
                pipeline_factory::makePipeline(_userPipeline,
                                               _sharedState->_pipeline->getContext(),
                                               pipeline_factory::kOptionsMinimal)
                    ->serializeToBson(opts);
            return buildUnionWithResult(Value(serializedPipeline),
                                        Value(opts.serializeIdentifier(_userNss.coll())));
        }

        MutableDocument spec;
        if (!_sharedState->_pipeline->getContext()
                 ->getNamespaceString()
                 .isCollectionlessAggregateNS()) {
            // Fully resolve the namespace when serializing for shard dispatch.
            const auto& serializeNss = _fromNsIsAView
                ? _sharedState->_pipeline->getContext()->getNamespaceString()
                : _userNss;
            spec["coll"] = Value(opts.serializeIdentifier(serializeNss.coll()));
        }
        // Strip $mergeCursors if present (injected by mongos post-dispatch; invalid on re-parse).
        const bool hasMergeCursors = !_sharedState->_pipeline->getSources().empty() &&
            _sharedState->_pipeline->getSources().front()->getSourceName() == "$mergeCursors"sv;
        if (hasMergeCursors) {
            // Re-parse from user pipeline to get the clean optimized form without $mergeCursors.
            // TODO SERVER-94227: we don't need to do any validation as part of this parsing pass.
            spec["pipeline"] =
                Value(pipeline_factory::makePipeline(_userPipeline,
                                                     _sharedState->_pipeline->getContext(),
                                                     pipeline_factory::kOptionsMinimal)
                          ->serializeToBson(opts));
        } else {
            spec["pipeline"] = Value(_sharedState->_pipeline->serializeToBson(opts));
        }
        appendIsHybridSearchFlag(spec, opts);
        return Value(DOC(getSourceName() << spec.freezeToValue()));
    }
}

// Extracting dependencies for the outer collection. Although, this method walks the inner pipeline,
// the field dependencies are not collected - only variable dependencies are.
DepsTracker::State DocumentSourceUnionWith::getDependencies(DepsTracker* deps) const {
    if (!_sharedState->_pipeline) {
        return DepsTracker::State::SEE_NEXT;
    }

    DepsTracker subDeps;
    // Get the subpipeline dependencies.
    for (auto&& source : _sharedState->_pipeline->getSources()) {
        source->getDependencies(&subDeps);
    }

    return DepsTracker::State::SEE_NEXT;
}

void DocumentSourceUnionWith::addVariableRefs(std::set<Variables::Id>* refs) const {
    // Add sub-pipeline variable dependencies. Do not add SEARCH_META as a dependency, since it is
    // scoped to one pipeline.
    std::set<Variables::Id> subPipeRefs;
    _sharedState->_pipeline->addVariableRefs(&subPipeRefs);
    for (auto&& varId : subPipeRefs) {
        if (varId != Variables::kSearchMetaId)
            refs->insert(varId);
    }
}

void DocumentSourceUnionWith::detachSourceFromOperationContext() {
    // We have an execution pipeline we're going to execute across multiple commands, so we need to
    // detach it from the operation context when it goes out of scope.
    if (_sharedState->_execPipeline) {
        _sharedState->_execPipeline->detachFromOperationContext();
    }
    // Some methods require pipeline to have a valid operation context. Normally, a pipeline and the
    // corresponding execution pipeline share the same expression context containing a pointer to
    // the operation context, but it might not be the case anymore when a pipeline is cloned with
    // another expression context.
    if (_sharedState->_pipeline) {
        _sharedState->_pipeline->detachFromOperationContext();
    }
}

void DocumentSourceUnionWith::reattachSourceToOperationContext(OperationContext* opCtx) {
    // We have an execution pipeline we're going to execute across multiple commands, so we need to
    // propagate the new operation context to the pipeline stages.
    if (_sharedState->_execPipeline) {
        _sharedState->_execPipeline->reattachToOperationContext(opCtx);
    }
    // Some methods require pipeline to have a valid operation context. Normally, a pipeline and the
    // corresponding execution pipeline share the same expression context containing a pointer to
    // the operation context, but it might not be the case anymore when a pipeline is cloned with
    // another expression context.
    if (_sharedState->_pipeline) {
        _sharedState->_pipeline->reattachToOperationContext(opCtx);
    }
}

bool DocumentSourceUnionWith::validateSourceOperationContext(const OperationContext* opCtx) const {
    return getExpCtx()->getOperationContext() == opCtx &&
        (!_sharedState->_pipeline || _sharedState->_pipeline->validateOperationContext(opCtx));
}

void DocumentSourceUnionWith::addInvolvedCollections(
    stdx::unordered_set<NamespaceString>* collectionNames) const {
    collectionNames->insert(_sharedState->_pipeline->getContext()->getNamespaceString());
    collectionNames->merge(_sharedState->_pipeline->getInvolvedCollections());
}

std::unique_ptr<Pipeline> DocumentSourceUnionWith::parsePipelineWithMaybeViewDefinition(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ResolvedNamespace& resolvedNs,
    std::vector<BSONObj> currentPipeline,
    const NamespaceString& userNss,
    bool isHybridSearch) {
    // We will call optimize() when finalizing the pipeline in 'doGetNext()'.
    auto opts = pipeline_factory::kDesugarOnly;
    opts.validator = assertAllStagesAllowedInUnionWith;

    boost::intrusive_ptr<ExpressionContext> subExpCtx = makeCopyForSubPipelineFromExpressionContext(
        expCtx, resolvedNs.getResolvedNamespace(), resolvedNs.getCollUUID(), userNss);
    subExpCtx->setInUnionWith(true);
    // The desugared mongot stage's injected 'view' field requires isHybridSearch on the
    // sub-pipeline's expCtx. 'currentPipeline' may already be desugared; those callers pass
    // isHybridSearch=true.
    if (isHybridSearch || hybrid_scoring_util::isHybridSearchPipeline(currentPipeline)) {
        subExpCtx->setIsHybridSearch();
    }
    if (resolvedNs.getResolvedNamespace().isTimeseriesBucketsCollection() &&
        isRawDataOperation(expCtx->getOperationContext())) {
        // Raw Data operations on timeseries collections operate without the timeseries view.
        return pipeline_factory::makePipeline(currentPipeline, subExpCtx, opts);
    }

    return pipeline_factory::makePipelineFromViewDefinition(
        subExpCtx, resolvedNs, std::move(currentPipeline), opts, userNss);
}

std::unique_ptr<Pipeline>
DocumentSourceUnionWith::parsePipelineFromStageParamsWithMaybeViewDefinition(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const ResolvedNamespace& resolvedNs,
    StageParamsPipeline stageParams,
    const std::vector<BSONObj>& rawPipeline,
    const NamespaceString& userNss) {
    boost::intrusive_ptr<ExpressionContext> subExpCtx = makeCopyForSubPipelineFromExpressionContext(
        expCtx, resolvedNs.getResolvedNamespace(), resolvedNs.getCollUUID(), userNss);
    subExpCtx->setInUnionWith(true);
    // The desugared mongot stage's injected 'view' field requires isHybridSearch on the
    // sub-pipeline's expCtx.
    // TODO SERVER-121094: remove once the $_internalHybridSearch marker is the single
    // hybrid-search signal and these per-site setIsHybridSearch() calls are no longer needed.
    if (hybrid_scoring_util::isHybridSearchPipeline(rawPipeline)) {
        subExpCtx->setIsHybridSearch();
    }

    auto opts = pipeline_factory::kDesugarOnly;
    opts.validator = assertAllStagesAllowedInUnionWith;
    return pipeline_factory::makePipelineFromViewDefinitionStageParams(
        subExpCtx, resolvedNs, std::move(stageParams), rawPipeline, userNss, opts);
}

}  // namespace mongo
