// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_source_rank_fusion.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_hybrid_scoring_util.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_rank_fusion.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_factory.h"
#include "mongo/db/pipeline/rank_fusion_pipeline_builder.h"
#include "mongo/db/query/allowed_contexts.h"
#include "mongo/db/query/query_feature_flags_gen.h"
#include "mongo/db/query/util/rank_fusion_util.h"
#include "mongo/util/string_map.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

REGISTER_LITE_PARSED_DOCUMENT_SOURCE(rankFusion,
                                     LiteParsedRankFusion::parse,
                                     AllowedWithApiStrict::kNeverInVersion1);

namespace {
DocumentSourceContainer rankFusionStageParamsToDocumentSourceFn(
    const std::unique_ptr<StageParams>& stageParams,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    auto* typedParams = dynamic_cast<RankFusionStageParams*>(stageParams.get());
    tassert(
        12192021, "Expected RankFusionStageParams in $rankFusion dispatch", typedParams != nullptr);
    // TODO SERVER-121094 Remove the createFromBson path when the feature flag is deleted.
    auto ifrCtx = expCtx->getIfrContext();
    auto hybridSearchFlagEnabled = ifrCtx &&
        ifrCtx->getSavedFlagValue(feature_flags::gFeatureFlagExtensionsInsideHybridSearch);
    if (hybridSearchFlagEnabled) {
        return DocumentSourceRankFusion::createFromStageParams(*typedParams, expCtx);
    }
    return DocumentSourceRankFusion::createFromBson(typedParams->getOriginalBson(), expCtx);
}
}  // namespace

REGISTER_STAGE_PARAMS_TO_DOCUMENT_SOURCE_MAPPING(rankFusion,
                                                 RankFusionStageParams::id,
                                                 rankFusionStageParamsToDocumentSourceFn);

/**
 * Checks that the input pipeline is a valid ranked pipeline. This means it is either one of
 * $search, $vectorSearch, $geoNear, $rankFusion, $scoreFusion (which have ordered output) or has an
 * explicit $sort stage. A ranked pipeline must also be a 'selection pipeline', which means no stage
 * can modify the documents in any way. Only stages that retrieve, limit, or order documents are
 * allowed.
 *
 * Input pipeline validation happens during the explicit validation phase at lite-parse time
 * (LiteParsedRankFusion::validate()) before parsing into a `Pipeline` object.
 */
std::map<std::string, std::unique_ptr<Pipeline>> parseSelectionPipelines(
    const RankFusionSpec& spec, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
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
        inputPipelines[innerPipelineBsonElem.fieldName()] = std::move(pipeline);
    }
    return inputPipelines;
}

std::list<boost::intrusive_ptr<DocumentSource>> DocumentSourceRankFusion::createFromStageParams(
    const RankFusionStageParams& params, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(ErrorCodes::QueryFeatureNotAllowed,
            "'featureFlagRankFusionBasic' must be enabled to use rankFusion",
            bypassRankFusionFCVGate || pExpCtx->isBasicRankFusionFeatureFlagEnabled());

    const auto& spec = params.getSpec();
    const auto& inputPipelines = params.buildInputPipelines(pExpCtx);

    StringMap<double> weights;
    const auto& combinationSpec = spec.getCombination();
    if (combinationSpec.has_value()) {
        weights = hybrid_scoring_util::validateWeights(
            combinationSpec->getWeights(), inputPipelines, kStageName);
    }

    // TODO SERVER-129346 This should create a DocumentSourceScoreFusion that simply computes query
    // shape.
    RankFusionPipelineBuilder builder(spec, weights);
    return builder.constructDesugaredOutput(inputPipelines, pExpCtx);
}

std::list<boost::intrusive_ptr<DocumentSource>> DocumentSourceRankFusion::createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {
    uassert(ErrorCodes::QueryFeatureNotAllowed,
            "'featureFlagRankFusionBasic' must be enabled to use rankFusion",
            bypassRankFusionFCVGate || pExpCtx->isBasicRankFusionFeatureFlagEnabled());

    uassert(ErrorCodes::FailedToParse,
            str::stream() << "The " << kStageName
                          << " stage specification must be an object, found "
                          << typeName(elem.type()),
            elem.type() == BSONType::object);

    auto spec = RankFusionSpec::parse(elem.embeddedObject(), IDLParserContext(kStageName));

    const auto& inputPipelines = parseSelectionPipelines(spec, pExpCtx);

    StringMap<double> weights;
    // If RankFusionCombinationSpec has no value (no weights specified), no work to do.
    const auto& combinationSpec = spec.getCombination();
    if (combinationSpec.has_value()) {
        weights = hybrid_scoring_util::validateWeights(
            combinationSpec->getWeights(), inputPipelines, kStageName);
    }

    // TODO SERVER-85426: Remove this check once all feature flags have been removed.
    if (spec.getScoreDetails()) {
        uassert(ErrorCodes::QueryFeatureNotAllowed,
                "'featureFlagRankFusionFull' must be enabled to use scoreDetails",
                isRankFusionFullEnabled());
    }

    RankFusionPipelineBuilder builder(spec, weights);

    std::list<boost::intrusive_ptr<DocumentSource>> outputStages =
        builder.constructDesugaredOutput(inputPipelines, pExpCtx);

    return outputStages;
}
}  // namespace mongo
