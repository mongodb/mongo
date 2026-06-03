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
#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/document_source_internal_document_results_and_metadata_gen.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

using boost::intrusive_ptr;

namespace {
exec::agg::StagePtr documentSourceInternalDocumentResultsAndMetadataToStageFn(
    const boost::intrusive_ptr<DocumentSource>& ds) {
    // TODO: SERVER-126343
    uasserted(ErrorCodes::NotImplemented,
              str::stream() << DocumentSourceInternalDocumentResultsAndMetadata::kStageName
                            << " execution is not yet implemented.");
}
}  // namespace

REGISTER_AGG_STAGE_MAPPING(_internalDocumentResultsAndMetadata,
                           DocumentSourceInternalDocumentResultsAndMetadata::id,
                           documentSourceInternalDocumentResultsAndMetadataToStageFn);

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
