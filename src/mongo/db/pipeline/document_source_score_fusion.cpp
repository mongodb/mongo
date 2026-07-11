// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_score_fusion.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_hybrid_scoring_util.h"
#include "mongo/db/pipeline/document_source_score_fusion_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_score_fusion.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/pipeline/score_fusion_pipeline_builder.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/query_feature_flags_gen.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

REGISTER_LITE_PARSED_DOCUMENT_SOURCE_WITH_FEATURE_FLAG(
    scoreFusion,
    LiteParsedScoreFusion::parse,
    AllowedWithApiStrict::kNeverInVersion1,
    &feature_flags::gFeatureFlagSearchHybridScoringFull);

namespace {
DocumentSourceContainer scoreFusionStageParamsToDocumentSourceFn(
    const std::unique_ptr<StageParams>& stageParams,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto* typedParams = dynamic_cast<ScoreFusionStageParams*>(stageParams.get());
    tassert(12192020,
            "Expected ScoreFusionStageParams in $scoreFusion dispatch",
            typedParams != nullptr);
    // TODO SERVER-121094 Remove the createFromBson path when the feature flag is deleted.
    auto ifrCtx = expCtx->getIfrContext();
    auto hybridSearchFlagEnabled = ifrCtx &&
        ifrCtx->getSavedFlagValue(feature_flags::gFeatureFlagExtensionsInsideHybridSearch);
    if (hybridSearchFlagEnabled) {
        return DocumentSourceScoreFusion::createFromStageParams(*typedParams, expCtx);
    }
    return DocumentSourceScoreFusion::createFromBson(typedParams->getOriginalBson(), expCtx);
}
}  // namespace

REGISTER_STAGE_PARAMS_TO_DOCUMENT_SOURCE_MAPPING(scoreFusion,
                                                 ScoreFusionStageParams::id,
                                                 scoreFusionStageParamsToDocumentSourceFn);

/**
 * Checks that the input pipeline is a valid scored pipeline. This means it is either one of
 * $search, $vectorSearch, $scoreFusion, $rankFusion (which have scored output) or has an explicit
 * $score stage. A scored pipeline must also be a 'selection pipeline', which means no stage can
 * modify the documents in any way. Only stages that retrieve, limit, or order documents are
 * allowed.
 *
 * Input pipeline validation happens during the explicit validation phase at lite-parse time
 * (LiteParsedScoreFusion::validate()) before parsing into a `Pipeline` object.
 */
std::map<std::string, std::unique_ptr<Pipeline>> parseScoredSelectionPipelines(
    const ScoreFusionSpec& spec, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    // It's important to use an ordered map here, so that we get stability in the desugaring =>
    // stability in the query shape.
    std::map<std::string, std::unique_ptr<Pipeline>> inputPipelines;
    for (const auto& innerPipelineBsonElem : spec.getInput().getPipelines()) {
        auto bsonPipeline = parsePipelineFromBSON(innerPipelineBsonElem);
        LiteParsedPipeline liteParsedPipeline(
            pExpCtx->getNamespaceString(),
            bsonPipeline,
            false,
            LiteParserOptions{.ifrContext = pExpCtx->getIfrContext()});
        auto pipeline = Pipeline::parseFromLiteParsed(liteParsedPipeline, pExpCtx);

        // Post-condition sanity check: LiteParsedScoreFusion::validate() already asserted the
        // input pipeline produces score metadata; confirm the fully-parsed pipeline agrees.
        tassert(
            10535800,
            "The metadata dependency tracker determined $scoreFusion input pipeline does not "
            "generate score metadata, despite the input pipeline stages being previously validated "
            "as such.",
            pipeline->generatesMetadataType(DocumentMetadataFields::kScore));

        inputPipelines[innerPipelineBsonElem.fieldName()] = std::move(pipeline);
    }
    return inputPipelines;
}

std::list<boost::intrusive_ptr<DocumentSource>> DocumentSourceScoreFusion::createFromStageParams(
    const ScoreFusionStageParams& params, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    const auto& spec = params.getSpec();
    const auto& inputPipelines = params.buildInputPipelines(pExpCtx);

    StringMap<double> weights;
    const auto& combinationSpec = spec.getCombination();
    if (combinationSpec.has_value() && combinationSpec->getWeights().has_value()) {
        weights = hybrid_scoring_util::validateWeights(
            combinationSpec->getWeights()->getOwned(), inputPipelines, kStageName);
    }

    // TODO SERVER-129346 This should create a DocumentSourceScoreFusion that simply computes query
    // shape.
    ScoreFusionPipelineBuilder builder(spec, weights);
    return builder.constructDesugaredOutput(inputPipelines, pExpCtx);
}

std::list<boost::intrusive_ptr<DocumentSource>> DocumentSourceScoreFusion::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(ErrorCodes::FailedToParse,
            str::stream() << "The " << kStageName
                          << " stage specification must be an object, found "
                          << typeName(elem.type()),
            elem.type() == BSONType::object);

    auto spec = ScoreFusionSpec::parse(elem.embeddedObject(), IDLParserContext(kStageName));

    const auto& inputPipelines = parseScoredSelectionPipelines(spec, pExpCtx);

    StringMap<double> weights;
    // If ScoreFusionCombinationSpec has no value (no weights specified), no work to do.
    const auto& combinationSpec = spec.getCombination();
    if (combinationSpec.has_value() && combinationSpec->getWeights().has_value()) {
        weights = hybrid_scoring_util::validateWeights(
            combinationSpec->getWeights()->getOwned(), inputPipelines, kStageName);
    }

    ScoreFusionPipelineBuilder builder(spec, weights);

    std::list<boost::intrusive_ptr<DocumentSource>> outputStages =
        builder.constructDesugaredOutput(inputPipelines, pExpCtx);

    return outputStages;
}
}  // namespace mongo
