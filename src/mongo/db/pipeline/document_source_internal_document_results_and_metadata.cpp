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

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/document_source_exchange.h"
#include "mongo/db/pipeline/document_source_internal_document_results_and_metadata_gen.h"
#include "mongo/db/pipeline/document_source_replace_root.h"
#include "mongo/db/pipeline/document_source_set_variable_from_subpipeline.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using boost::intrusive_ptr;

namespace {

constexpr int kMetaResultStreamType = 1;

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

    // Create the copy before Exchange construction, Exchange ctor calls
    // detachFromOperationContext() on its inner pipelines, which nulls expCtx->_operationContext,
    // and Pipeline::create for the meta pipeline needs a live opCtx.
    const auto metaExpCtx = hasMeta
        ? makeCopyFromExpressionContext(expCtx, expCtx->getNamespaceString(), expCtx->getUUID())
        : boost::intrusive_ptr<ExpressionContext>{};

    // Exchange ctor detaches expCtx->opCtx (nulls it) as part of detaching its inner pipeline.
    // Restore it on scope exit so the outer consumer stages sharing this expCtx have a live opCtx.
    auto* const opCtx = expCtx->getOperationContext();
    ON_BLOCK_EXIT([&] { expCtx->setOperationContext(opCtx); });
    auto exchange = make_intrusive<exec::agg::Exchange>(
        opCtx,
        buildStreamRoutingSpec(hasMeta),
        Pipeline::create({docResultsAndMeta.getSourceStage()}, expCtx));

    auto makeReplaceRootDs = [](const boost::intrusive_ptr<ExpressionContext>& ctx)
        -> boost::intrusive_ptr<DocumentSource> {
        return DocumentSourceReplaceRoot::create(
            ctx,
            ExpressionFieldPath::parse(ctx.get(), "$payload", ctx->variablesParseState),
            "'payload' expression",
            SbeCompatibility::notCompatible);
    };

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
    const auto& spec = params.getSpec();

    auto sourceList = DocumentSource::parse(expCtx, spec.getSource());
    tassert(12615003,
            str::stream() << kStageName << " 'source' must parse to exactly one stage",
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
    if (auto metaSpec = spec.getMetadata()) {
        tassert(12615005,
                str::stream() << kStageName << " 'metadata.as' must be '"
                              << Variables::kSearchMetaName << "', got '" << metaSpec->getAs()
                              << "'",
                metaSpec->getAs() == Variables::kSearchMetaName);
        metadata = std::move(metaSpec);
    }

    return {make_intrusive<DocumentSourceInternalDocumentResultsAndMetadata>(
        expCtx, std::move(sourceStage), std::move(metadata), spec.getReturnCursor())};
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
    const SerializationOptions& opts) const {
    DocumentSourceResultsAndMetadataSpec spec;
    spec.setSource(_sourceStage->serialize(opts).getDocument().toBson());
    spec.setMetadata(_metadata);
    spec.setReturnCursor(_returnCursor);
    return Value(Document{{getSourceName(), spec.toBSON(opts)}});
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
        // TODO: SERVER-126183 invoke _sourceStage->skipStream(kMetadataResult) once
        // skip_stream is added to MongoExtensionLogicalAggStageVTable in api.h.
    }
    return std::next(itr);
}

}  // namespace mongo
