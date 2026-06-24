/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_internal_document_results_and_metadata.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/document_source_exchange.h"
#include "mongo/db/pipeline/document_source_internal_document_results_and_metadata_gen.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/document_source_set_variable_from_subpipeline.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/pipeline/stage_params_to_document_source_registry.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using boost::intrusive_ptr;

namespace {

constexpr int kMetaResultStreamType = 1;

boost::intrusive_ptr<DocumentSource> makeReplaceRootDs(
    const boost::intrusive_ptr<ExpressionContext>& ctx) {
    return DocumentSourceReplaceRoot::create(
        ctx,
        ExpressionFieldPath::parse(ctx.get(), "$payload", ctx->variablesParseState),
        "'payload' expression",
        SbeCompatibility::notCompatible);
}

ExchangeSpec buildStreamRoutingSpec(bool hasMetadata) {
    std::vector<BSONObj> boundaries{BSON("" << MINKEY)};
    std::vector<int> consumerIds{0};
    if (hasMetadata) {
        boundaries.push_back(BSON("" << kMetaResultStreamType));
        consumerIds.push_back(1);
    }
    boundaries.push_back(BSON("" << MAXKEY));

    ExchangeSpec spec;
    spec.setPolicy(ExchangePolicyEnum::kKeyRange);
    spec.setKey(BSON("_streamType" << 1));
    spec.setConsumers(static_cast<int>(consumerIds.size()));
    spec.setBoundaries(std::move(boundaries));
    spec.setConsumerIds(std::move(consumerIds));
    return spec;
}

exec::agg::StageExpansion documentSourceInternalDocumentResultsAndMetadataToStagesFn(
    const boost::intrusive_ptr<DocumentSource>& ds) {
    auto& docResultsAndMeta = checked_cast<DocumentSourceInternalDocumentResultsAndMetadata&>(*ds);
    auto expCtx = docResultsAndMeta.getExpCtx();
    const bool hasMeta = docResultsAndMeta.getMetadata().has_value();

    // Give the inner source and meta pipelines their own expCtx copies. The Exchange detaches its
    // inner pipeline on every getNext, which nulls that expCtx's opCtx. Sharing the outer expCtx
    // would leak that null to the outer consumer stages.
    auto* const opCtx = expCtx->getOperationContext();
    const auto docExpCtx =
        makeCopyFromExpressionContext(expCtx, expCtx->getNamespaceString(), expCtx->getUUID());
    const auto metaExpCtx = hasMeta
        ? makeCopyFromExpressionContext(expCtx, expCtx->getNamespaceString(), expCtx->getUUID())
        : boost::intrusive_ptr<ExpressionContext>{};

    auto exchange = make_intrusive<exec::agg::Exchange>(
        opCtx,
        buildStreamRoutingSpec(hasMeta),
        // Clone the source onto docExpCtx so the stage's expCtx matches the inner pipeline's.
        Pipeline::create({docResultsAndMeta.getSourceStage()->clone(docExpCtx)}, docExpCtx));

    exec::agg::StageExpansion stages;
    stages.push_back(exec::agg::buildStage(
        make_intrusive<DocumentSourceExchange>(expCtx, exchange, 0, nullptr)));
    stages.push_back(exec::agg::buildStage(makeReplaceRootDs(expCtx)));

    if (!hasMeta)
        return stages;

    auto metaConsumer = make_intrusive<DocumentSourceExchange>(metaExpCtx, exchange, 1, nullptr);
    auto metaPipeline = Pipeline::create({metaConsumer, makeReplaceRootDs(metaExpCtx)}, metaExpCtx);
    metaPipeline->pipelineType = CursorTypeEnum::SearchMetaResult;

    // Sharded path: stash the meta pipeline on the DS so run_aggregate.cpp can retrieve it and
    // register it as a secondary cursor (the router uses pipelineType to identify it). We stash
    // rather than split early (like createExchangePipelinesIfNeeded) to keep all Exchange setup
    // in this translation function.
    // Standalone path: consume metadata in-process by binding it to $$SEARCH_META.
    if (docResultsAndMeta.getReturnCursor()) {
        docResultsAndMeta.stashAdditionalCursorPipeline(std::move(metaPipeline));
    } else {
        stages.push_back(exec::agg::buildStage(DocumentSourceSetVariableFromSubPipeline::create(
            expCtx, std::move(metaPipeline), Variables::kSearchMetaId)));
    }

    return stages;
}

}  // namespace

REGISTER_AGG_STAGES_MAPPING(_internalDocumentResultsAndMetadata,
                            DocumentSourceInternalDocumentResultsAndMetadata::id,
                            documentSourceInternalDocumentResultsAndMetadataToStagesFn);

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(_internalDocumentResultsAndMetadata,
                                     InternalDocumentResultsAndMetadataLiteParsed::parse,
                                     AllowedWithApiStrict::kInternal);

DocumentSourceContainer internalDocumentResultsAndMetadataStageParamsToDocumentSourceFn(
    const std::unique_ptr<StageParams>& stageParams,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto* typedParams =
        dynamic_cast<InternalDocumentResultsAndMetadataStageParams*>(stageParams.get());
    tassert(12615006,
            "Expected InternalDocumentResultsAndMetadataStageParams in "
            "$_internalDocumentResultsAndMetadata dispatch",
            typedParams != nullptr);
    return DocumentSourceInternalDocumentResultsAndMetadata::createFromStageParams(*typedParams,
                                                                                   expCtx);
}

REGISTER_STAGE_PARAMS_TO_DOCUMENT_SOURCE_MAPPING(
    _internalDocumentResultsAndMetadata,
    InternalDocumentResultsAndMetadataStageParams::id,
    internalDocumentResultsAndMetadataStageParamsToDocumentSourceFn);

ALLOCATE_DOCUMENT_SOURCE_ID(_internalDocumentResultsAndMetadata,
                            DocumentSourceInternalDocumentResultsAndMetadata::id);

DocumentSourceInternalDocumentResultsAndMetadata::DocumentSourceInternalDocumentResultsAndMetadata(
    const intrusive_ptr<ExpressionContext>& expCtx,
    intrusive_ptr<DocumentSource> source,
    boost::optional<MetadataBindSpec> metadata,
    bool returnCursor)
    : DocumentSource(kStageName, expCtx),
      _sourceStage(std::move(source)),
      _metadata(std::move(metadata)),
      _returnCursor(returnCursor) {}

intrusive_ptr<DocumentSourceInternalDocumentResultsAndMetadata>
DocumentSourceInternalDocumentResultsAndMetadata::create(
    const intrusive_ptr<ExpressionContext>& expCtx,
    intrusive_ptr<DocumentSource> source,
    boost::optional<MetadataBindSpec> metadata,
    bool returnCursor) {
    return make_intrusive<DocumentSourceInternalDocumentResultsAndMetadata>(
        expCtx, std::move(source), std::move(metadata), returnCursor);
}

DocumentSourceContainer DocumentSourceInternalDocumentResultsAndMetadata::createFromStageParams(
    const InternalDocumentResultsAndMetadataStageParams& params,
    const intrusive_ptr<ExpressionContext>& expCtx) {
    // Build the inner source DocumentSource from its lite-parsed representation.
    const auto& innerStages = params.getSourcePipeline()->getStages();
    tassert(12615003,
            str::stream() << kStageName << " 'source' must lite-parse to exactly one stage",
            innerStages.size() == 1);
    auto sourceList = buildDocumentSource(*innerStages.front(), expCtx);
    tassert(12615010,
            str::stream() << kStageName << " 'source' must build exactly one DocumentSource",
            sourceList.size() == 1);
    auto sourceStage = std::move(sourceList.front());
    auto sourceConstraints = sourceStage->constraints();
    tassert(12615004,
            str::stream() << kStageName
                          << " 'source' must be an initial source stage that generates its own "
                             "documents (requiredPosition == kFirst, requiresInputDocSource == "
                             "false)",
            sourceConstraints.requiredPosition == PositionRequirement::kFirst &&
                !sourceConstraints.requiresInputDocSource);

    boost::optional<MetadataBindSpec> metadata;
    if (const auto& metaSpec = params.getMetadata()) {
        tassert(12615005,
                str::stream() << kStageName << " 'metadata.as' must be '$$SEARCH_META', got '"
                              << metaSpec->getAs() << "'",
                metaSpec->getAs() == Variables::kSearchMetaName);
        metadata = metaSpec;
    }

    auto stage = make_intrusive<DocumentSourceInternalDocumentResultsAndMetadata>(
        expCtx, std::move(sourceStage), std::move(metadata), params.getReturnCursor());
    // The extension's distributed-plan callback is carried through the LiteParsed -> StageParams
    // handoff and attached here so distributedPlanLogic() can produce a sharded merge plan.
    if (const auto& provider = params.getShardedPlanProvider()) {
        stage->setShardedPlanProvider(provider);
    }
    return {std::move(stage)};
}

DepsTracker::State DocumentSourceInternalDocumentResultsAndMetadata::getDependencies(
    DepsTracker* deps) const {
    // Forward to the inner source: this stage adds no dependency behavior of its own beyond
    // exposing the source's metadata availability declarations to downstream stages.
    return _sourceStage->getDependencies(deps);
}

StageConstraints DocumentSourceInternalDocumentResultsAndMetadata::constraints(
    PipelineSplitState pipeState) const {
    // Inherit most constraints from the source stage, override facet and transaction since this
    // stage expands into Exchange infrastructure that cannot run in those contexts.
    auto constraints = _sourceStage->constraints(pipeState);
    constraints.facetRequirement = StageConstraints::FacetRequirement::kNotAllowed;
    constraints.transactionRequirement = StageConstraints::TransactionRequirement::kNotAllowed;
    return constraints;
}

Value DocumentSourceInternalDocumentResultsAndMetadata::serialize(
    const query_shape::SerializationOptions& opts) const {
    DocumentSourceResultsAndMetadataSpec spec;
    spec.setSource(_sourceStage->serialize(opts).getDocument().toBson());
    spec.setMetadata(_metadata);
    spec.setReturnCursor(_returnCursor);
    return Value(Document{{getSourceName(), spec.toBSON(opts)}});
}

void DocumentSourceInternalDocumentResultsAndMetadata::serializeToArray(
    std::vector<Value>& array, const query_shape::SerializationOptions& opts) const {
    // This stage itself serializes to a single entry, which lines up with the Exchange consumer
    // that heads the exec lowering.
    array.push_back(serialize(opts));

    // Below executionStats verbosity, PlanExecutorPipeline::writeExplainOps() does not merge in the
    // exec pipeline, so a single entry is correct.
    if (!opts.verbosity || *opts.verbosity < ExplainOptions::Verbosity::kExecStats) {
        return;
    }

    // At executionStats verbosity the exec pipeline is merged in positionally by mergeExplains().
    // This single DocumentSource lowers to multiple exec::agg stages, so build the follow-on stages
    // and let them serialize themselves.
    const auto& expCtx = getExpCtx();
    makeReplaceRootDs(expCtx)->serializeToArray(array, opts);

    if (_metadata && !_returnCursor) {
        // The real inner pipeline ([Exchange consumer, $replaceRoot]) is only built during
        // lowering and can't be reconstructed here. Emit a minimal placeholder with just the
        // variable name so the entry count stays aligned for mergeExplains().
        array.push_back(Value(Document{
            {DocumentSourceSetVariableFromSubPipeline::kStageName,
             Document{
                 {"setVariable",
                  Value("$$" + Variables::getBuiltinVariableName(Variables::kSearchMetaId))}}}}));
    }
}

boost::intrusive_ptr<DocumentSource> DocumentSourceInternalDocumentResultsAndMetadata::clone(
    const intrusive_ptr<ExpressionContext>& expCtx) const {
    auto cloned = DocumentSourceInternalDocumentResultsAndMetadata::create(
        expCtx, _sourceStage->clone(expCtx), _metadata, _returnCursor);
    if (_shardedPlanProvider) {
        cloned->setShardedPlanProvider(_shardedPlanProvider);
    }
    return cloned;
}

DocumentSourceContainer::iterator DocumentSourceInternalDocumentResultsAndMetadata::optimizeAt(
    DocumentSourceContainer::iterator itr, DocumentSourceContainer* container) {
    tassert(12615007, "Expecting DocumentSource iterator pointing to this stage.", *itr == this);
    // Shards only see their local pipeline view, so $$SEARCH_META refs in the merge
    // pipeline are invisible to them. Only the router runs metadata elision.
    if (getExpCtx()->getNeedsMerge() || !_metadata) {
        return std::next(itr);
    }

    const bool referencesSearchMeta =
        std::any_of(std::next(itr), container->end(), [](const auto& stage) {
            return search_helpers::hasReferenceToSearchMeta(*stage);
        });
    if (!referencesSearchMeta) {
        LOGV2_DEBUG(12615008,
                    4,
                    "Eliding metadata stream as no downstream stage references $$SEARCH_META.");
        _metadata = boost::none;
        _sourceStage->skipMetadataStream();
    }
    return std::next(itr);
}

boost::optional<DocumentSource::DistributedPlanLogic>
DocumentSourceInternalDocumentResultsAndMetadata::distributedPlanLogic(
    const DistributedPlanContext* ctx) {
    // This stage acts as transport layer, DPL info is provided by configured source stage via the
    // callback. Without it, this stage has no distributed plan logic to provide.
    if (!_shardedPlanProvider) {
        return boost::none;
    }

    DistributedPlanLogic logic;
    logic.shardsStage = this;
    logic.needsSplit = false;
    logic.canMovePast = canMovePastDuringSplit;

    const auto& expCtx = getExpCtx();
    const auto& dplResult = _shardedPlanProvider(expCtx.get());
    tassert(12625501,
            str::stream() << kStageName << " DPL callback must return a non-empty sort pattern",
            !dplResult.resultsSortPattern.isEmpty());
    logic.mergeSortPattern = dplResult.resultsSortPattern.getOwned();

    // No metadata: only merge-sort is needed, no merging pipeline for metadata stream.
    if (!_metadata) {
        return logic;
    }

    // Flip _returnCursor so the shard-side stage stashes the metadata stream as a secondary cursor
    // that the router reads, rather than consuming it in-process via $setVariableFromSubPipeline.
    // Informational probes (e.g. requiredToRunOnRouter, stageCanRunInParallel) use ctx=nullptr and
    // shouldn't cause side-effects, whereas SplitPipeline::split() always passes a non-null ctx.
    if (ctx != nullptr) {
        _returnCursor = true;
    }

    // Wrap the merge pipeline in $setVariableFromSubPipeline to bind
    // $$SEARCH_META on the router.
    tassert(12625502,
            str::stream() << kStageName
                          << " DPL callback must return a non-empty merge pipeline when "
                             "metadata is present",
            !dplResult.metaMergePipeline.empty());
    logic.mergingStages = {DocumentSourceSetVariableFromSubPipeline::create(
        expCtx,
        pipeline_factory::makePipeline(
            dplResult.metaMergePipeline, expCtx, pipeline_factory::kOptionsMinimal),
        Variables::kSearchMetaId)};

    return logic;
}

bool DocumentSourceInternalDocumentResultsAndMetadata::canMovePastDuringSplit(
    const DocumentSource& ds) {
    return search_helpers::canMovePastDuringSplit(ds);
}

}  // namespace mongo
